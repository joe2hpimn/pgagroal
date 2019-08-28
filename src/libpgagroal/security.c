/*
 * Copyright (C) 2019 Red Hat
 * 
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice, this list
 * of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright notice, this
 * list of conditions and the following disclaimer in the documentation and/or other
 * materials provided with the distribution.
 * 
 * 3. Neither the name of the copyright holder nor the names of its contributors may
 * be used to endorse or promote products derived from this software without specific
 * prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 * TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* pgagroal */
#include <pgagroal.h>
#include <logging.h>
#include <message.h>
#include <pool.h>
#include <security.h>
#include <server.h>
#include <utils.h>

#define ZF_LOG_TAG "security"
#include <zf_log.h>

/* system */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int get_auth_type(struct message* msg, int* auth_type);
static int compare_auth_response(struct message* orig, struct message* response, int auth_type);

int
pgagroal_authenticate(int client_fd, void* shmem, int* slot)
{
   int status = MESSAGE_STATUS_ERROR;
   int server_fd = -1;
   int auth_index = 0;
   int auth_type = -1;
   int auth_response = -1;
   struct configuration* config;
   struct message* msg = NULL;
   struct message* auth_msg = NULL;
   int32_t request;
   char* username = NULL;
   char* database = NULL;

   config = (struct configuration*)shmem;

   /* Receive client calls - at any point if client exits return AUTH_FAILURE */
   status = pgagroal_read_block_message(client_fd, &msg);
   if (status != MESSAGE_STATUS_OK)
   {
      goto error;
   }

   request = pgagroal_get_request(msg);

   /* GSS request: 80877104 */
   if (request == 80877104)
   {
      ZF_LOGD("GSS request from client: %d", client_fd);
      status = pgagroal_write_notice(client_fd);
      if (status != MESSAGE_STATUS_OK)
      {
         goto error;
      }
      pgagroal_free_message(msg);

      status = pgagroal_read_block_message(client_fd, &msg);
      if (status != MESSAGE_STATUS_OK)
      {
         goto error;
      }
      request = pgagroal_get_request(msg);
   }

   /* SSL request: 80877103 */
   if (request == 80877103)
   {
      ZF_LOGD("SSL request from client: %d", client_fd);
      status = pgagroal_write_notice(client_fd);
      if (status != MESSAGE_STATUS_OK)
      {
         goto error;
      }
      pgagroal_free_message(msg);

      status = pgagroal_read_block_message(client_fd, &msg);
      if (status != MESSAGE_STATUS_OK)
      {
         goto error;
      }
      request = pgagroal_get_request(msg);
   }

   /* 196608 -> Ok */
   if (request == 196608)
   {
      /* Extract parameters: username / database */
      ZF_LOGV("authenticate: username/database (%d)", client_fd);
      pgagroal_extract_username_database(msg, &username, &database);

      /* Get connection */
      if (pgagroal_get_connection(shmem, username, database, slot))
      {
         /* Pool full */
         ZF_LOGD("authenticate: pool is full");
         pgagroal_write_pool_full(client_fd);
         pgagroal_write_empty(client_fd);
         goto error;
      }
      server_fd = config->connections[*slot].fd;

      if (config->connections[*slot].has_security != -1)
      {
         ZF_LOGD("authenticate: getting pooled connection");
         pgagroal_free_message(msg);

         pgagroal_create_message(&config->connections[*slot].security_messages[0],
                                 config->connections[*slot].security_lengths[0],
                                 &auth_msg);

         status = pgagroal_write_message(client_fd, auth_msg);
         if (status != MESSAGE_STATUS_OK)
         {
            goto error;
         }
         pgagroal_free_copy_message(auth_msg);
         auth_msg = NULL;

         if (config->connections[*slot].has_security != 0)
         {
            status = pgagroal_read_block_message(client_fd, &msg);
            if (status != MESSAGE_STATUS_OK)
            {
               goto error;
            }
         
            pgagroal_create_message(&config->connections[*slot].security_messages[1],
                                    config->connections[*slot].security_lengths[1],
                                    &auth_msg);

            if (compare_auth_response(auth_msg, msg, config->connections[*slot].has_security))
            {
               /* Bad password */
               pgagroal_write_bad_password(client_fd, username);

               /* Make sure that the client know that we are "closing" the connection */
               pgagroal_write_empty(client_fd);

               goto error;
            }

            pgagroal_free_copy_message(auth_msg);
            auth_msg = NULL;

            pgagroal_create_message(&config->connections[*slot].security_messages[2],
                                    config->connections[*slot].security_lengths[2],
                                    &auth_msg);
         
            status = pgagroal_write_message(client_fd, auth_msg);
            if (status != MESSAGE_STATUS_OK)
            {
               goto error;
            }
            pgagroal_free_copy_message(auth_msg);
            auth_msg = NULL;
         }
         
         ZF_LOGD("authenticate: got pooled connection (%d)", *slot);
      }
      else
      {
         ZF_LOGD("authenticate: creating pooled connection");

         /* Send auth request to PostgreSQL */
         ZF_LOGV("authenticate: client auth request (%d)", client_fd);
         status = pgagroal_write_message(server_fd, msg);
         if (status != MESSAGE_STATUS_OK)
         {
            goto error;
         }
         pgagroal_free_message(msg);
      
         /* Keep response, and send response to client */
         ZF_LOGV("authenticate: server auth request (%d)", server_fd);
         status = pgagroal_read_block_message(server_fd, &msg);
         if (status != MESSAGE_STATUS_OK)
         {
            goto error;
         }

         get_auth_type(msg, &auth_type);
         ZF_LOGV("authenticate: auth type %d", auth_type);

         /* "Supported" security models: */
         /*   trust (0) */
         /*   password (3) */
         /*   md5 (5) */
         /*   scram-sha-256 (10) */
         if (auth_type == -1)
         {
            pgagroal_write_message(client_fd, msg);
            pgagroal_write_empty(client_fd);
            goto error;
         }
         else if (auth_type != 0 && auth_type != 3 && auth_type != 5 && auth_type != 10)
         {
            ZF_LOGI("Unsupported security model: %d", auth_type);
            pgagroal_write_unsupported_security_model(client_fd, username);
            pgagroal_write_empty(client_fd);
            goto error;
         }

         for (int i = 0; i < NUMBER_OF_SECURITY_MESSAGES; i++)
         {
            memset(&config->connections[*slot].security_messages[i], 0, SECURITY_BUFFER_SIZE);
         }
         
         if (msg->length > SECURITY_BUFFER_SIZE)
         {
            ZF_LOGE("Security message too large: %ld", msg->length);
            goto error;
         }

         config->connections[*slot].security_lengths[auth_index] = msg->length;
         memcpy(&config->connections[*slot].security_messages[auth_index], msg->data, msg->length);
         auth_index++;

         status = pgagroal_write_message(client_fd, msg);
         if (status != MESSAGE_STATUS_OK)
         {
            goto error;
         }
         pgagroal_free_message(msg);

         /* Non-trust clients */
         if (auth_type != 0)
         {
            /* Receive client response, keep it, and send it to PostgreSQL */
            status = pgagroal_read_block_message(client_fd, &msg);
            if (status != MESSAGE_STATUS_OK)
            {
               goto error;
            }

            if (msg->length > SECURITY_BUFFER_SIZE)
            {
               ZF_LOGE("Security message too large: %ld", msg->length);
               goto error;
            }

            config->connections[*slot].security_lengths[auth_index] = msg->length;
            memcpy(&config->connections[*slot].security_messages[auth_index], msg->data, msg->length);
            auth_index++;

            status = pgagroal_write_message(server_fd, msg);
            if (status != MESSAGE_STATUS_OK)
            {
               goto error;
            }
            pgagroal_free_message(msg);

            status = pgagroal_read_block_message(server_fd, &msg);
            if (status != MESSAGE_STATUS_OK)
            {
               goto error;
            }

            if (auth_type == 10)
            {
               if (msg->length > SECURITY_BUFFER_SIZE)
               {
                  ZF_LOGE("Security message too large: %ld", msg->length);
                  goto error;
               }
               
               config->connections[*slot].security_lengths[auth_index] = msg->length;
               memcpy(&config->connections[*slot].security_messages[auth_index], msg->data, msg->length);
               auth_index++;

               status = pgagroal_write_message(client_fd, msg);
               if (status != MESSAGE_STATUS_OK)
               {
                  goto error;
               }
               pgagroal_free_message(msg);
               
               status = pgagroal_read_block_message(client_fd, &msg);
               if (status != MESSAGE_STATUS_OK)
               {
                  goto error;
               }

               if (msg->length > SECURITY_BUFFER_SIZE)
               {
                  ZF_LOGE("Security message too large: %ld", msg->length);
                  goto error;
               }

               config->connections[*slot].security_lengths[auth_index] = msg->length;
               memcpy(&config->connections[*slot].security_messages[auth_index], msg->data, msg->length);
               auth_index++;

               status = pgagroal_write_message(server_fd, msg);
               if (status != MESSAGE_STATUS_OK)
               {
                  goto error;
               }
               pgagroal_free_message(msg);

               status = pgagroal_read_block_message(server_fd, &msg);
               if (status != MESSAGE_STATUS_OK)
               {
                  goto error;
               }
            }

            /* Ok: Keep the response, send it to the client, and exit authenticate() */
            get_auth_type(msg, &auth_response);
            ZF_LOGV("authenticate: auth response %d", auth_response);

            if (auth_response == 0)
            {
               if (msg->length > SECURITY_BUFFER_SIZE)
               {
                  ZF_LOGE("Security message too large: %ld", msg->length);
                  goto error;
               }

               config->connections[*slot].security_lengths[auth_index] = msg->length;
               memcpy(&config->connections[*slot].security_messages[auth_index], msg->data, msg->length);

               config->connections[*slot].has_security = auth_type;
            }
         
            status = pgagroal_write_message(client_fd, msg);
            if (status != MESSAGE_STATUS_OK)
            {
               goto error;
            }
            pgagroal_free_message(msg);

            if (auth_response != 0)
            {
               goto error;
            }
         }
         else
         {
            /* Trust */
            config->connections[*slot].has_security = auth_type;
         }

         if (config->servers[config->connections[*slot].server].primary == SERVER_NOTINIT ||
             config->servers[config->connections[*slot].server].primary == SERVER_NOTINIT_PRIMARY)
         {
            ZF_LOGD("Verify server mode: %d", config->connections[*slot].server);
            pgagroal_update_server_state(shmem, *slot, server_fd);
            pgagroal_server_status(shmem);
         }

         ZF_LOGV("authenticate: has_security %d", config->connections[*slot].has_security);
         ZF_LOGD("authenticate: created pooled connection (%d)", *slot);
      }

      free(username);
      free(database);
      
      ZF_LOGD("authenticate: SUCCESS");
      return AUTH_SUCCESS;
   }

 error:
   pgagroal_free_message(msg);
   if (auth_msg)
      pgagroal_free_copy_message(auth_msg);

   free(username);
   free(database);

   ZF_LOGD("authenticate: FAILURE");
   return AUTH_FAILURE;
}


static int
get_auth_type(struct message* msg, int* auth_type)
{
   int32_t length;
   int32_t type = -1;
   int offset;

   if (msg->kind != 'R')
      return 1;

   length = pgagroal_read_int32(msg->data + 1);
   type = pgagroal_read_int32(msg->data + 5);
   offset = 9;

   switch (type)
   {
      case 0:
         ZF_LOGV("Backend: R - Success");
         break;
      case 2:
         ZF_LOGV("Backend: R - KerberosV5");
         break;
      case 3:
         ZF_LOGV("Backend: R - CleartextPassword");
         break;
      case 5:
         ZF_LOGV("Backend: R - MD5Password");
         ZF_LOGV("             Salt %02hhx%02hhx%02hhx%02hhx", pgagroal_read_byte(msg->data + 9) & 0xFF,
                 pgagroal_read_byte(msg->data + 10) & 0xFF, pgagroal_read_byte(msg->data + 11) & 0xFF,
                 pgagroal_read_byte(msg->data + 12) & 0xFF);
         break;
      case 6:
         ZF_LOGV("Backend: R - SCMCredential");
         break;
      case 7:
         ZF_LOGV("Backend: R - GSS");
         break;
      case 8:
         ZF_LOGV("Backend: R - GSSContinue");
         break;
      case 9:
         ZF_LOGV("Backend: R - SSPI");
         break;
      case 10:
         ZF_LOGV("Backend: R - SASL");
         while (offset < length - 8)
         {
            char* mechanism = pgagroal_read_string(msg->data + offset);
            ZF_LOGV("             %s", mechanism);
            offset += strlen(mechanism) + 1;
         }
         break;
      case 11:
         ZF_LOGV("Backend: R - SASLContinue");
         ZF_LOGV_MEM(msg->data + offset, length - 8, "Message %p:", (const void *)msg->data + offset);
         break;
      case 12:
         ZF_LOGV("Backend: R - SASLFinal");
         ZF_LOGV_MEM(msg->data + offset, length - 8, "Message %p:", (const void *)msg->data + offset);
         offset += length - 8;

         if (offset < msg->length)
         {
            signed char peek = pgagroal_read_byte(msg->data + offset);
            switch (peek)
            {
               case 'R':
                  type = pgagroal_read_int32(msg->data + offset + 5);
                  break;
               default:
                  break;
            }
         }

         break;
      default:
         break;
   }

   *auth_type = type;

   return 0;
}

static int
compare_auth_response(struct message* orig, struct message* response, int auth_type)
{
   switch (auth_type)
   {
      case 3:
      case 5:
         return strcmp(pgagroal_read_string(orig->data + 5), pgagroal_read_string(response->data + 5));
         break;
      case 10:
         return memcmp(orig->data, response->data, orig->length);
         break;
      default:
         break;
   }

   return 1;
}
