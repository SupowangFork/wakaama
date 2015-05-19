/*******************************************************************************
 *
 * Copyright (c) 2013, 2014 Intel Corporation and others.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    http://www.eclipse.org/legal/epl-v10.html
 * The Eclipse Distribution License is available at
 *    http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    David Navarro, Intel Corporation - initial API and implementation
 *    domedambrosio - Please refer to git log
 *    Simon Bernard - Please refer to git log
 *    Toby Jaffey - Please refer to git log
 *    Julien Vermillard - Please refer to git log
 *    Bosch Software Innovations GmbH - Please refer to git log
 *
 *******************************************************************************/

/*
 Copyright (c) 2013, 2014 Intel Corporation

 Redistribution and use in source and binary forms, with or without modification,
 are permitted provided that the following conditions are met:

     * Redistributions of source code must retain the above copyright notice,
       this list of conditions and the following disclaimer.
     * Redistributions in binary form must reproduce the above copyright notice,
       this list of conditions and the following disclaimer in the documentation
       and/or other materials provided with the distribution.
     * Neither the name of Intel Corporation nor the names of its contributors
       may be used to endorse or promote products derived from this software
       without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 THE POSSIBILITY OF SUCH DAMAGE.

 David Navarro <david.navarro@intel.com>

*/


#include "liblwm2m.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>
#include <inttypes.h>

#include "commandline.h"
#include "connection.h"
#include "IPSO.h"

#ifdef WITH_TINYDTLS
#include <tinydtls/global.h>
#include <tinydtls/debug.h>
#include <tinydtls/dtls.h>

#define PSK_IDENTITY    "IPSO_Interop"
#define PSK_KEY         "Not_So_Secret_Key"
#endif

/*
 * ensure sync with: er_coap_13.h COAP_MAX_PACKET_SIZE!
 * or internals.h LWM2M_MAX_PACKET_SIZE!
 */
#define MAX_PACKET_SIZE 198

static int g_quit = 0;
static bool g_ipso = true;

typedef struct _ipso_object_
{
    struct _ipso_object_ * next;
    uint16_t clientId;
    lwm2m_client_object_t * objectP;
    char templateStr[512];
    char * outputStr;
} ipso_object_t;

typedef struct
{
    int sock;
    lwm2m_context_t * lwm2mH;
    ipso_object_t * objectList;
} internal_data_t;


static void prv_print_error(uint8_t status)
{
    fprintf(stdout, "Error: ");
    print_status(stdout, status);
    fprintf(stdout, "\r\n");
}

#ifdef WITH_TINYDTLS
// This is the write callback for tinyDTLS
static int prv_dtls_send(struct dtls_context_t * dtlsH,
                         session_t * dst,
                         uint8 * data,
                         size_t length)
{
    internal_data_t * dataP = (internal_data_t *)dtls_get_app_data(dtlsH);
    fprintf(stderr, "%s\r\n", __FUNCTION__);

    return sendto(dataP->sock, data, length, MSG_DONTWAIT, &dst->addr.sa, dst->size);
}

// This is the buffer send callback for wakaama
static uint8_t prv_buffer_send(void * sessionH,
                               uint8_t * buffer,
                               size_t length,
                               void * userData)
{
    dtls_context_t * dtlsH = (dtls_context_t *)userData;
    session_t * dst = (session_t *)sessionH;
    int nbSent;
    size_t offset;
    fprintf(stderr, "%s\r\n", __FUNCTION__);

    offset = 0;
    while (offset != length)
    {
        nbSent = dtls_write(dtlsH, dst, buffer + offset, length - offset);
        if (nbSent == -1) return COAP_500_INTERNAL_SERVER_ERROR;
        offset += nbSent;
    }
    return COAP_NO_ERROR;
}

// This is the read callback for tinyDTLS
static int prv_dtls_read(dtls_context_t * dtlsH,
                         session_t * src,
                         uint8 * data,
                         size_t length)
{
    internal_data_t * dataP = (internal_data_t *)dtls_get_app_data(dtlsH);
    dtls_peer_t * peerP;
    fprintf(stderr, "%s\r\n", __FUNCTION__);

    peerP = dtls_get_peer(dtlsH, src);
    if (peerP != NULL)
    {
        lwm2m_handle_packet(dataP->lwm2mH, data, length, (void *)src);
    }
}

// This is the get_psk_info callback for tinyDTLS
static int prv_dtls_get_psk(dtls_context_t * dtlsH,
                            const session_t * session,
                            dtls_credentials_type_t type,
                            const unsigned char * identity,
                            size_t identityLength,
                            unsigned char * result,
                            size_t resultLength)
{
    fprintf(stderr, "%s\r\n", __FUNCTION__);
    switch (type)
    {
    case DTLS_PSK_HINT:
        return 0;

    case DTLS_PSK_IDENTITY:
        if (resultLength < strlen(PSK_IDENTITY))
        {
            return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
        }
        memcpy(result, PSK_IDENTITY, strlen(PSK_IDENTITY));
        return strlen(PSK_IDENTITY);

    case DTLS_PSK_KEY:
        if (identityLength != strlen(PSK_IDENTITY)
         || strcmp(identity, PSK_IDENTITY) != 0)
        {
            return dtls_alert_fatal_create(DTLS_ALERT_ILLEGAL_PARAMETER);
        }
        if (resultLength < strlen(PSK_KEY))
        {
            return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
        }
        memcpy(result, PSK_KEY, strlen(PSK_KEY));
        return strlen(PSK_KEY);

    default:
        break;
    }

    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
}

#else
static uint8_t prv_buffer_send(void * sessionH,
                               uint8_t * buffer,
                               size_t length,
                               void * userdata)
{
    connection_t * connP = (connection_t*) sessionH;

    if (-1 == connection_send(connP, buffer, length))
    {
        return COAP_500_INTERNAL_SERVER_ERROR;
    }
    return COAP_NO_ERROR;
}
#endif

static char * prv_dump_binding(lwm2m_binding_t binding)
{
    switch (binding)
    {
    case BINDING_UNKNOWN:
        return "Not specified";
    case BINDING_U:
        return "UDP";
    case BINDING_UQ:
        return "UDP queue mode";
    case BINDING_S:
        return "SMS";
    case BINDING_SQ:
        return "SMS queue mode";
    case BINDING_US:
        return "UDP plus SMS";
    case BINDING_UQS:
        return "UDP queue mode plus SMS";
    default:
        return "";
    }
}

static void prv_dump_client(lwm2m_client_t * targetP)
{
    lwm2m_client_object_t * objectP;

    fprintf(stdout, "Client #%d:\r\n", targetP->internalID);
    fprintf(stdout, "\tname: \"%s\"\r\n", targetP->name);
    fprintf(stdout, "\tbinding: \"%s\"\r\n", prv_dump_binding(targetP->binding));
    if (targetP->msisdn) fprintf(stdout, "\tmsisdn: \"%s\"\r\n", targetP->msisdn);
    if (targetP->altPath) fprintf(stdout, "\talternative path: \"%s\"\r\n", targetP->altPath);
    fprintf(stdout, "\tlifetime: %d sec\r\n", targetP->lifetime);
    fprintf(stdout, "\tobjects: ");
    for (objectP = targetP->objectList; objectP != NULL ; objectP = objectP->next)
    {
        if (objectP->instanceList == NULL)
        {
            fprintf(stdout, "/%d, ", objectP->id);
        }
        else
        {
            lwm2m_list_t * instanceP;

            for (instanceP = objectP->instanceList; instanceP != NULL ; instanceP = instanceP->next)
            {
                fprintf(stdout, "/%d/%d, ", objectP->id, instanceP->id);
            }
        }
    }
    fprintf(stdout, "\r\n");
}

static void prv_output_clients(char * buffer,
                               void * user_data)
{
    lwm2m_context_t * lwm2mH = (lwm2m_context_t *) user_data;
    lwm2m_client_t * targetP;

    targetP = lwm2mH->clientList;

    if (targetP == NULL)
    {
        fprintf(stdout, "No client.\r\n");
        return;
    }

    for (targetP = lwm2mH->clientList ; targetP != NULL ; targetP = targetP->next)
    {
        prv_dump_client(targetP);
    }
}

static int prv_read_id(char * buffer,
                       uint16_t * idP)
{
    int nb;
    int value;

    nb = sscanf(buffer, "%d", &value);
    if (nb == 1)
    {
        if (value < 0 || value > LWM2M_MAX_ID)
        {
            nb = 0;
        }
        else
        {
            *idP = value;
        }
    }

    return nb;
}

static void prv_result_callback(uint16_t clientID,
                                lwm2m_uri_t * uriP,
                                int status,
                                uint8_t * data,
                                int dataLength,
                                void * userData)
{
    fprintf(stdout, "\r\nClient #%d %d", clientID, uriP->objectId);
    if (LWM2M_URI_IS_SET_INSTANCE(uriP))
        fprintf(stdout, "/%d", uriP->instanceId);
    else if (LWM2M_URI_IS_SET_RESOURCE(uriP))
        fprintf(stdout, "/");
    if (LWM2M_URI_IS_SET_RESOURCE(uriP))
            fprintf(stdout, "/%d", uriP->resourceId);
    fprintf(stdout, " : ");
    print_status(stdout, status);
    fprintf(stdout, "\r\n");

    if (data != NULL)
    {
        fprintf(stdout, "%d bytes received:\r\n", dataLength);
        if (LWM2M_URI_IS_SET_RESOURCE(uriP))
        {
            output_buffer(stdout, data, dataLength, 1);
        }
        else
        {
            output_tlv(stdout, data, dataLength, 1);
        }
    }

    fprintf(stdout, "\r\n> ");
    fflush(stdout);
}

static void prv_notify_callback(uint16_t clientID,
                                lwm2m_uri_t * uriP,
                                int count,
                                uint8_t * data,
                                int dataLength,
                                void * userData)
{
    fprintf(stdout, "\r\nNotify from client #%d /%d", clientID, uriP->objectId);
    if (LWM2M_URI_IS_SET_INSTANCE(uriP))
        fprintf(stdout, "/%d", uriP->instanceId);
    else if (LWM2M_URI_IS_SET_RESOURCE(uriP))
        fprintf(stdout, "/");
    if (LWM2M_URI_IS_SET_RESOURCE(uriP))
            fprintf(stdout, "/%d", uriP->resourceId);
    fprintf(stdout, " number %d\r\n", count);

    if (data != NULL)
    {
        fprintf(stdout, "%d bytes received:\r\n", dataLength);
        output_buffer(stdout, data, dataLength, 0);
    }

    fprintf(stdout, "\r\n> ");
    fflush(stdout);
}

static void prv_read_client(char * buffer,
                            void * user_data)
{
    lwm2m_context_t * lwm2mH = (lwm2m_context_t *) user_data;
    uint16_t clientId;
    lwm2m_uri_t uri;
    char* end = NULL;
    int result;

    result = prv_read_id(buffer, &clientId);
    if (result != 1) goto syntax_error;

    buffer = get_next_arg(buffer, &end);
    if (buffer[0] == 0) goto syntax_error;

    result = lwm2m_stringToUri(buffer, end - buffer, &uri);
    if (result == 0) goto syntax_error;

    if (!check_end_of_args(end)) goto syntax_error;

    result = lwm2m_dm_read(lwm2mH, clientId, &uri, prv_result_callback, NULL);

    if (result == 0)
    {
        fprintf(stdout, "OK");
    }
    else
    {
        prv_print_error(result);
    }
    return;

syntax_error:
    fprintf(stdout, "Syntax error !");
}

static void prv_write_client(char * buffer,
                             void * user_data)
{
    lwm2m_context_t * lwm2mH = (lwm2m_context_t *) user_data;
    uint16_t clientId;
    lwm2m_uri_t uri;
    char * end = NULL;
    int result;

    result = prv_read_id(buffer, &clientId);
    if (result != 1) goto syntax_error;

    buffer = get_next_arg(buffer, &end);
    if (buffer[0] == 0) goto syntax_error;

    result = lwm2m_stringToUri(buffer, end - buffer, &uri);
    if (result == 0) goto syntax_error;

    buffer = get_next_arg(end, &end);
    if (buffer[0] == 0) goto syntax_error;

    if (!check_end_of_args(end)) goto syntax_error;

    result = lwm2m_dm_write(lwm2mH, clientId, &uri, (uint8_t *)buffer, end - buffer, prv_result_callback, NULL);

    if (result == 0)
    {
        fprintf(stdout, "OK");
    }
    else
    {
        prv_print_error(result);
    }
    return;

syntax_error:
    fprintf(stdout, "Syntax error !");
}


static void prv_exec_client(char * buffer,
                            void * user_data)
{
    lwm2m_context_t * lwm2mH = (lwm2m_context_t *) user_data;
    uint16_t clientId;
    lwm2m_uri_t uri;
    char * end = NULL;
    int result;

    result = prv_read_id(buffer, &clientId);
    if (result != 1) goto syntax_error;

    buffer = get_next_arg(buffer, &end);
    if (buffer[0] == 0) goto syntax_error;

    result = lwm2m_stringToUri(buffer, end - buffer, &uri);
    if (result == 0) goto syntax_error;

    buffer = get_next_arg(end, &end);


    if (buffer[0] == 0)
    {
        result = lwm2m_dm_execute(lwm2mH, clientId, &uri, NULL, 0, prv_result_callback, NULL);
    }
    else
    {
        if (!check_end_of_args(end)) goto syntax_error;

        result = lwm2m_dm_execute(lwm2mH, clientId, &uri, (uint8_t *)buffer, end - buffer, prv_result_callback, NULL);
    }

    if (result == 0)
    {
        fprintf(stdout, "OK");
    }
    else
    {
        prv_print_error(result);
    }
    return;

syntax_error:
    fprintf(stdout, "Syntax error !");
}

static void prv_create_client(char * buffer,
                              void * user_data)
{
    lwm2m_context_t * lwm2mH = (lwm2m_context_t *) user_data;
    uint16_t clientId;
    lwm2m_uri_t uri;
    char * end = NULL;
    int result;
    int64_t value;
    uint8_t temp_buffer[MAX_PACKET_SIZE];
    int temp_length = 0;


    //Get Client ID
    result = prv_read_id(buffer, &clientId);
    if (result != 1) goto syntax_error;

    //Get Uri
    buffer = get_next_arg(buffer, &end);
    if (buffer[0] == 0) goto syntax_error;

    result = lwm2m_stringToUri(buffer, end - buffer, &uri);
    if (result == 0) goto syntax_error;

    //Get Data to Post
    buffer = get_next_arg(end, &end);
    if (buffer[0] == 0) goto syntax_error;

    if (!check_end_of_args(end)) goto syntax_error;

   // TLV

   /* Client dependent part   */

    if (uri.objectId == 1024)
    {
        result = lwm2m_PlainTextToInt64((uint8_t *)buffer, end - buffer, &value);
        temp_length = lwm2m_intToTLV(LWM2M_TYPE_RESOURCE, value, (uint16_t) 1, temp_buffer, MAX_PACKET_SIZE);
    }
   /* End Client dependent part*/

    //Create
    result = lwm2m_dm_create(lwm2mH, clientId,&uri, temp_buffer, temp_length, prv_result_callback, NULL);

    if (result == 0)
    {
        fprintf(stdout, "OK");
    }
    else
    {
        prv_print_error(result);
    }
    return;

syntax_error:
    fprintf(stdout, "Syntax error !");
}

static void prv_delete_client(char * buffer,
                              void * user_data)
{
    lwm2m_context_t * lwm2mH = (lwm2m_context_t *) user_data;
    uint16_t clientId;
    lwm2m_uri_t uri;
    char* end = NULL;
    int result;

    result = prv_read_id(buffer, &clientId);
    if (result != 1) goto syntax_error;

    buffer = get_next_arg(buffer, &end);
    if (buffer[0] == 0) goto syntax_error;

    result = lwm2m_stringToUri(buffer, end - buffer, &uri);
    if (result == 0) goto syntax_error;

    if (!check_end_of_args(end)) goto syntax_error;

    result = lwm2m_dm_delete(lwm2mH, clientId, &uri, prv_result_callback, NULL);

    if (result == 0)
    {
        fprintf(stdout, "OK");
    }
    else
    {
        prv_print_error(result);
    }
    return;

syntax_error:
    fprintf(stdout, "Syntax error !");
}

static void prv_observe_client(char * buffer,
                               void * user_data)
{
    lwm2m_context_t * lwm2mH = (lwm2m_context_t *) user_data;
    uint16_t clientId;
    lwm2m_uri_t uri;
    char* end = NULL;
    int result;

    result = prv_read_id(buffer, &clientId);
    if (result != 1) goto syntax_error;

    buffer = get_next_arg(buffer, &end);
    if (buffer[0] == 0) goto syntax_error;

    result = lwm2m_stringToUri(buffer, end - buffer, &uri);
    if (result == 0) goto syntax_error;

    if (!check_end_of_args(end)) goto syntax_error;

    result = lwm2m_observe(lwm2mH, clientId, &uri, prv_notify_callback, NULL);

    if (result == 0)
    {
        fprintf(stdout, "OK");
    }
    else
    {
        prv_print_error(result);
    }
    return;

syntax_error:
    fprintf(stdout, "Syntax error !");
}

static void prv_cancel_client(char * buffer,
                              void * user_data)
{
    lwm2m_context_t * lwm2mH = (lwm2m_context_t *) user_data;
    uint16_t clientId;
    lwm2m_uri_t uri;
    char* end = NULL;
    int result;

    result = prv_read_id(buffer, &clientId);
    if (result != 1) goto syntax_error;

    buffer = get_next_arg(buffer, &end);
    if (buffer[0] == 0) goto syntax_error;

    result = lwm2m_stringToUri(buffer, end - buffer, &uri);
    if (result == 0) goto syntax_error;

    if (!check_end_of_args(end)) goto syntax_error;

    result = lwm2m_observe_cancel(lwm2mH, clientId, &uri, prv_result_callback, NULL);

    if (result == 0)
    {
        fprintf(stdout, "OK");
    }
    else
    {
        prv_print_error(result);
    }
    return;

syntax_error:
    fprintf(stdout, "Syntax error !");
}

static void prv_ipso_callback(uint16_t clientID,
                              lwm2m_uri_t * uriP,
                              int status,
                              uint8_t * data,
                              int dataLength,
                              void * userData)
{
    ipso_object_t * objectP = (ipso_object_t *)userData;
    char * tempStr = NULL;
    int i;

    if (!LWM2M_URI_IS_SET_RESOURCE(uriP)) return;
    if (data == NULL) return;

    for (i = 0 ; i < dataLength ; i++)
    {
        if (!isprint(data[i]))
        {
            break;
        }
    }

    if (i == dataLength)
    {
        tempStr = (char *) lwm2m_malloc(dataLength + 1);
        if (tempStr == NULL) return;

        memcpy(tempStr, data, dataLength);
        tempStr[dataLength] = 0;
    }
    else
    {
        lwm2m_tlv_t * tlvP = NULL;
        int size;

        size = lwm2m_tlv_parse(data, dataLength, &tlvP);
        if (size == 1)
        {
            double value;

            if (1 == lwm2m_tlv_decode_float(tlvP, &value))
            {
                tempStr = (char *)lwm2m_malloc(32);
                if (tempStr == NULL) return;

                snprintf(tempStr, 32, "%lf", value);
            }
        }
    }

    if (tempStr == NULL)
    {
        fprintf(stderr, "Invalid data: \r\n");
        output_buffer(stderr, data, dataLength, 0);
    }
    else
    {
        if (objectP->outputStr != NULL) lwm2m_free(objectP->outputStr);
        objectP->outputStr = tempStr;
    }
}

static void prv_add_ipso_client(internal_data_t * dataP,
                                lwm2m_client_t * targetP)
{
    lwm2m_client_object_t * cltObjP;

    cltObjP = targetP->objectList;
    while (cltObjP != NULL)
    {
        if (cltObjP->id >= IPSO_Digital_Input_OBJ_ID
         && cltObjP->id <= IPSO_Barometer_OBJ_ID)
        {
            lwm2m_list_t * instP;

            instP = cltObjP->instanceList;

            while (instP != NULL)
            {
                ipso_object_t * objectP;
                lwm2m_uri_t uri;

                objectP = (ipso_object_t *)lwm2m_malloc(sizeof(ipso_object_t));
                if (objectP == NULL) return;

                memset(objectP, 0, sizeof(ipso_object_t));
                objectP->clientId = targetP->internalID;
                objectP->objectP = cltObjP;

                uri.flag = LWM2M_URI_FLAG_OBJECT_ID | LWM2M_URI_FLAG_INSTANCE_ID | LWM2M_URI_FLAG_RESOURCE_ID;
                uri.objectId = cltObjP->id;
                uri.instanceId = instP->id;

                switch (cltObjP->id)
                {
                case IPSO_Digital_Input_OBJ_ID:
                    uri.resourceId = IPSO_Digital_Input_State_RES_ID;
                    snprintf(objectP->templateStr, 512, "Client \"%s\" Digital Input #%d: ", targetP->name, instP->id);
                    lwm2m_observe(dataP->lwm2mH, targetP->internalID, &uri, prv_ipso_callback, (void *)objectP);
                    break;

                case IPSO_Digital_Output_OBJ_ID:
                    uri.resourceId = IPSO_Digital_Output_State_RES_ID;
                    snprintf(objectP->templateStr, 512, "Client \"%s\" Digital Output #%d: ", targetP->name, instP->id);
                    lwm2m_observe(dataP->lwm2mH, targetP->internalID, &uri, prv_ipso_callback, (void *)objectP);
                    break;

                case IPSO_Analogue_Input_OBJ_ID:
                    uri.resourceId = IPSO_Analog_Input_Current_Value_RES_ID;
                    snprintf(objectP->templateStr, 512, "Client \"%s\" Analog Input #%d: ", targetP->name, instP->id);
                    lwm2m_observe(dataP->lwm2mH, targetP->internalID, &uri, prv_ipso_callback, (void *)objectP);
                    break;

                case IPSO_Analogue_Output_OBJ_ID:
                    uri.resourceId = IPSO_Analog_Output_Current_Value_RES_ID;
                    snprintf(objectP->templateStr, 512, "Client \"%s\" Analog Output #%d: ", targetP->name, instP->id);
                    lwm2m_observe(dataP->lwm2mH, targetP->internalID, &uri, prv_ipso_callback, (void *)objectP);
                    break;

                case IPSO_Generic_Sensor_OBJ_ID:
                    uri.resourceId = IPSO_Sensor_Value_RES_ID;
                    snprintf(objectP->templateStr, 512, "Client \"%s\" Generic Sensor #%d: ", targetP->name, instP->id);
                    lwm2m_observe(dataP->lwm2mH, targetP->internalID, &uri, prv_ipso_callback, (void *)objectP);
                    break;

                case IPSO_Illuminance_Sensor_OBJ_ID:
                    uri.resourceId = IPSO_Sensor_Value_RES_ID;
                    snprintf(objectP->templateStr, 512, "Client \"%s\" Illuminance Sensor #%d: ", targetP->name, instP->id);
                    lwm2m_observe(dataP->lwm2mH, targetP->internalID, &uri, prv_ipso_callback, (void *)objectP);
                    break;

                case IPSO_Presence_Sensor_OBJ_ID:
                    uri.resourceId = IPSO_Digital_Input_State_RES_ID;
                    snprintf(objectP->templateStr, 512, "Client \"%s\" Presence Sensor #%d: ", targetP->name, instP->id);
                    lwm2m_observe(dataP->lwm2mH, targetP->internalID, &uri, prv_ipso_callback, (void *)objectP);
                    break;

                case IPSO_Temperature_Sensor_OBJ_ID:
                    uri.resourceId = IPSO_Sensor_Value_RES_ID;
                    snprintf(objectP->templateStr, 512, "Client \"%s\" Temperature Sensor #%d: ", targetP->name, instP->id);
                    lwm2m_observe(dataP->lwm2mH, targetP->internalID, &uri, prv_ipso_callback, (void *)objectP);
                    break;

                case IPSO_Humidity_Sensor_OBJ_ID:
                    uri.resourceId = IPSO_Sensor_Value_RES_ID;
                    snprintf(objectP->templateStr, 512, "Client \"%s\" Humidity Sensor #%d: ", targetP->name, instP->id);
                    lwm2m_observe(dataP->lwm2mH, targetP->internalID, &uri, prv_ipso_callback, (void *)objectP);
                    break;

                case IPSO_Power_Measurement_OBJ_ID:
                    uri.resourceId = IPSO_Instantaneous_active_power_RES_ID;
                    snprintf(objectP->templateStr, 512, "Client \"%s\" Power measurement #%d: ", targetP->name, instP->id);
                    lwm2m_observe(dataP->lwm2mH, targetP->internalID, &uri, prv_ipso_callback, (void *)objectP);
                    break;

                case IPSO_Actuation_OBJ_ID:
                    uri.resourceId = IPSO_On_Off_RES_ID;
                    snprintf(objectP->templateStr, 512, "Client \"%s\" Actuation #%d: ", targetP->name, instP->id);
                    lwm2m_observe(dataP->lwm2mH, targetP->internalID, &uri, prv_ipso_callback, (void *)objectP);
                    break;

                case IPSO_Set_Point_OBJ_ID:
                    uri.resourceId = IPSO_SetPoint_Value_RES_ID;
                    snprintf(objectP->templateStr, 512, "Client \"%s\" Set Point #%d: ", targetP->name, instP->id);
                    lwm2m_observe(dataP->lwm2mH, targetP->internalID, &uri, prv_ipso_callback, (void *)objectP);
                    break;

                case IPSO_Load_Control_OBJ_ID:
                    // TODO:
                    continue;
                    break;

                case IPSO_Light_Control_OBJ_ID:
                    uri.resourceId = IPSO_On_Off_RES_ID;
                    snprintf(objectP->templateStr, 512, "Client \"%s\" Light Control #%d: ", targetP->name, instP->id);
                    lwm2m_observe(dataP->lwm2mH, targetP->internalID, &uri, prv_ipso_callback, (void *)objectP);
                    break;

                case IPSO_Power_Control_OBJ_ID:
                    uri.resourceId = IPSO_On_Off_RES_ID;
                    snprintf(objectP->templateStr, 512, "Client \"%s\" Power Control #%d: ", targetP->name, instP->id);
                    lwm2m_observe(dataP->lwm2mH, targetP->internalID, &uri, prv_ipso_callback, (void *)objectP);
                    break;

                case IPSO_Accelerometer_OBJ_ID:
                    uri.resourceId = IPSO_X_Value_RES_ID;
                    snprintf(objectP->templateStr, 512, "Client \"%s\" Accelerometer X Value #%d: ", targetP->name, instP->id);
                    lwm2m_observe(dataP->lwm2mH, targetP->internalID, &uri, prv_ipso_callback, (void *)objectP);
                    break;

                case IPSO_Magnetometer_OBJ_ID:
                    uri.resourceId = IPSO_X_Value_RES_ID;
                    snprintf(objectP->templateStr, 512, "Client \"%s\" Magnetometer X Value #%d: ", targetP->name, instP->id);
                    lwm2m_observe(dataP->lwm2mH, targetP->internalID, &uri, prv_ipso_callback, (void *)objectP);
                    break;

                case IPSO_Barometer_OBJ_ID:
                    uri.resourceId = IPSO_Sensor_Value_RES_ID;
                    snprintf(objectP->templateStr, 512, "Client \"%s\" Barometer #%d: ", targetP->name, instP->id);
                    lwm2m_observe(dataP->lwm2mH, targetP->internalID, &uri, prv_ipso_callback, (void *)objectP);
                    break;
                }
                objectP->next = dataP->objectList;
                dataP->objectList = objectP;

                instP = instP->next;
            }
        }
        cltObjP = cltObjP->next;
    }
}

static void prv_remove_ipso_client(internal_data_t * dataP,
                                   uint16_t clientID)
{
    ipso_object_t * parentP;

    if (dataP->objectList == NULL) return;

    parentP = dataP->objectList;
    while (parentP->next != NULL)
    {
        if (parentP->next->clientId == clientID)
        {
            ipso_object_t * objectP;

            objectP = parentP->next;
            parentP->next = objectP->next;
            if (objectP->outputStr) lwm2m_free(objectP->outputStr);
            lwm2m_free(objectP);
        }
        else
        {
            parentP = parentP->next;
        }
    }
    if (dataP->objectList->clientId == clientID)
    {
        ipso_object_t * objectP;

        objectP = dataP->objectList;
        dataP->objectList = dataP->objectList->next;
        if (objectP->outputStr) lwm2m_free(objectP->outputStr);
        lwm2m_free(objectP);
    }
}

static void prv_ipso_display(internal_data_t * dataP)
{
    ipso_object_t * objectP;

    if (dataP->objectList == NULL) return;
    fprintf(stdout, "\r\n======== IPSO ========\r\n");
    objectP = dataP->objectList;
    while (objectP != NULL)
    {
        fprintf(stdout, objectP->templateStr);
        if (objectP->outputStr) fprintf(stdout, objectP->outputStr);
        fprintf(stdout, "\r\n");

        objectP = objectP->next;
    }
    fprintf(stdout, "======================\r\n");
}

static void prv_monitor_callback(uint16_t clientID,
                                 lwm2m_uri_t * uriP,
                                 int status,
                                 uint8_t * data,
                                 int dataLength,
                                 void * userData)
{
    internal_data_t * dataP = (internal_data_t *) userData;
    lwm2m_client_t * targetP;

    switch (status)
    {
    case COAP_201_CREATED:
        fprintf(stdout, "\r\nNew client #%d registered.\r\n", clientID);

        targetP = (lwm2m_client_t *)lwm2m_list_find((lwm2m_list_t *)dataP->lwm2mH->clientList, clientID);

        prv_dump_client(targetP);

        prv_remove_ipso_client(dataP, clientID);
        prv_add_ipso_client(dataP, targetP);
        break;

    case COAP_202_DELETED:
        fprintf(stdout, "\r\nClient #%d unregistered.\r\n", clientID);
        prv_remove_ipso_client(dataP, clientID);
        break;

    case COAP_204_CHANGED:
        fprintf(stdout, "\r\nClient #%d updated.\r\n", clientID);

        targetP = (lwm2m_client_t *)lwm2m_list_find((lwm2m_list_t *)dataP->lwm2mH->clientList, clientID);

        prv_dump_client(targetP);

        prv_remove_ipso_client(dataP, clientID);
        prv_add_ipso_client(dataP, targetP);
        break;

    default:
        fprintf(stdout, "\r\nMonitor callback called with an unknown status: %d.\r\n", status);
        break;
    }

    fprintf(stdout, "\r\n> ");
    fflush(stdout);
}


static void prv_quit(char * buffer,
                     void * user_data)
{
    g_quit = 1;
}

static void prv_switch_ipso(char * buffer,
                            void * user_data)
{
    g_ipso = g_ipso?false:true;
    fprintf(stdout, "\r\nIPSO UI set to %s\r\n", g_ipso?"ON":"OFF");
}

void handle_sigint(int signum)
{
    prv_quit(NULL, NULL);
}

void print_usage(void)
{
    fprintf(stderr, "Usage: lwm2mserver\r\n");
    fprintf(stderr, "Launch a LWM2M server on localhost port "LWM2M_STANDARD_PORT_STR".\r\n\n");
}


int main(int argc, char *argv[])
{
    fd_set readfds;
    struct timeval tv;
    int result;
    int i;
#ifdef WITH_TINYDTLS
    dtls_context_t * dtlsH = NULL;
    dtls_handler_t dtlsCb =
    {
        .write = prv_dtls_send,
        .read  = prv_dtls_read,
        .event = NULL,
    #ifdef DTLS_PSK
        .get_psk_info = prv_dtls_get_psk,
    #endif
    #ifdef DTLS_ECC
        .get_ecdsa_key = NULL,
        .verify_ecdsa_key = NULL,
    #endif
    };
#else
    connection_t * connList = NULL;
#endif
    internal_data_t data;

    command_desc_t commands[] =
    {
            {"list", "List registered clients.", NULL, prv_output_clients, NULL},
            {"read", "Read from a client.", " read CLIENT# URI\r\n"
                                            "   CLIENT#: client number as returned by command 'list'\r\n"
                                            "   URI: uri to read such as /3, /3//2, /3/0/2, /1024/11, /1024//1\r\n"
                                            "Result will be displayed asynchronously.", prv_read_client, NULL},
            {"write", "Write to a client.", " write CLIENT# URI DATA\r\n"
                                            "   CLIENT#: client number as returned by command 'list'\r\n"
                                            "   URI: uri to write to such as /3, /3//2, /3/0/2, /1024/11, /1024//1\r\n"
                                            "   DATA: data to write\r\n"
                                            "Result will be displayed asynchronously.", prv_write_client, NULL},
            {"exec", "Execute a client resource.", " exec CLIENT# URI\r\n"
                                            "   CLIENT#: client number as returned by command 'list'\r\n"
                                            "   URI: uri of the resource to execute such as /3/0/2\r\n"
                                            "Result will be displayed asynchronously.", prv_exec_client, NULL},
            {"del", "Delete a client Object instance.", " del CLIENT# URI\r\n"
                                            "   CLIENT#: client number as returned by command 'list'\r\n"
                                            "   URI: uri of the instance to delete such as /1024/11\r\n"
                                            "Result will be displayed asynchronously.", prv_delete_client, NULL},
            {"create", "create an Object instance.", " create CLIENT# URI DATA\r\n"
                                            "   CLIENT#: client number as returned by command 'list'\r\n"
                                            "   URI: uri to which create the Object Instance such as /1024, /1024/45 \r\n"
                                            "   DATA: data to initialize the new Object Instance (0-255 for object 1024) \r\n"
                                            "Result will be displayed asynchronously.", prv_create_client, NULL},
            {"observe", "Observe from a client.", " observe CLIENT# URI\r\n"
                                            "   CLIENT#: client number as returned by command 'list'\r\n"
                                            "   URI: uri to observe such as /3, /3/0/2, /1024/11\r\n"
                                            "Result will be displayed asynchronously.", prv_observe_client, NULL},
            {"cancel", "Cancel an observe.", " cancel CLIENT# URI\r\n"
                                            "   CLIENT#: client number as returned by command 'list'\r\n"
                                            "   URI: uri on which to cancel an observe such as /3, /3/0/2, /1024/11\r\n"
                                            "Result will be displayed asynchronously.", prv_cancel_client, NULL},
            {"ipso", "Toggle IPSO UI.", NULL, prv_switch_ipso, NULL},


            {"q", "Quit the server.", NULL, prv_quit, NULL},

            COMMAND_END_LIST
    };

    memset(&data, 0, sizeof(internal_data_t));

    data.sock = create_socket(LWM2M_STANDARD_PORT_STR);
    if (data.sock < 0)
    {
        fprintf(stderr, "Error opening socket: %d\r\n", errno);
        return -1;
    }

#ifdef WITH_TINYDTLS
    dtls_init();
    dtlsH = dtls_new_context((void *)&data);
    if (dtlsH == NULL)
    {
        fprintf(stderr, "dtls_new_context() failed\r\n");
        return -1;
    }
    dtls_set_handler(dtlsH, &dtlsCb);
    dtls_set_log_level(0);

    data.lwm2mH = lwm2m_init(NULL, prv_buffer_send, (void *)dtlsH);
#else
    data.lwm2mH = lwm2m_init(NULL, prv_buffer_send, NULL);
#endif
    if (NULL == data.lwm2mH)
    {
        fprintf(stderr, "lwm2m_init() failed\r\n");
        return -1;
    }

    signal(SIGINT, handle_sigint);

    for (i = 0 ; commands[i].name != NULL ; i++)
    {
        commands[i].userData = (void *)data.lwm2mH;
    }
    fprintf(stdout, "> "); fflush(stdout);

    lwm2m_set_monitoring_callback(data.lwm2mH, prv_monitor_callback, (void *)&data);

    while (0 == g_quit)
    {
        FD_ZERO(&readfds);
        FD_SET(data.sock, &readfds);
        FD_SET(STDIN_FILENO, &readfds);

        tv.tv_sec = 60;
        tv.tv_usec = 0;

        result = lwm2m_step(data.lwm2mH, &(tv.tv_sec));
        if (result != 0)
        {
            fprintf(stderr, "lwm2m_step() failed: 0x%X\r\n", result);
            return -1;
        }

        result = select(FD_SETSIZE, &readfds, 0, 0, &tv);

        if ( result < 0 )
        {
            if (errno != EINTR)
            {
              fprintf(stderr, "Error in select(): %d\r\n", errno);
            }
        }
        else if (result > 0)
        {
            uint8_t buffer[MAX_PACKET_SIZE];
            int numBytes;

            if (FD_ISSET(data.sock, &readfds))
            {
                struct sockaddr_storage addr;
                socklen_t addrLen;

                addrLen = sizeof(addr);
                numBytes = recvfrom(data.sock, buffer, MAX_PACKET_SIZE, 0, (struct sockaddr *)&addr, &addrLen);

                if (numBytes == -1)
                {
                    fprintf(stderr, "Error in recvfrom(): %d\r\n", errno);
                }
                else
                {
                    char s[INET6_ADDRSTRLEN];
                    in_port_t port;
#ifdef WITH_TINYDTLS
                    session_t session;
#else
                    connection_t * connP;
#endif

					s[0] = 0;
                    if (AF_INET == addr.ss_family)
                    {
                        struct sockaddr_in *saddr = (struct sockaddr_in *)&addr;
                        inet_ntop(saddr->sin_family, &saddr->sin_addr, s, INET6_ADDRSTRLEN);
                        port = saddr->sin_port;
                    }
                    else if (AF_INET6 == addr.ss_family)
                    {
                        struct sockaddr_in6 *saddr = (struct sockaddr_in6 *)&addr;
                        inet_ntop(saddr->sin6_family, &saddr->sin6_addr, s, INET6_ADDRSTRLEN);
                        port = saddr->sin6_port;
                    }

                    fprintf(stderr, "%d bytes received from [%s]:%hu\r\n", numBytes, s, ntohs(port));
                    output_buffer(stderr, buffer, numBytes, 0);

#ifdef WITH_TINYDTLS
                    memset(&session, 0, sizeof(session_t));
                    session.size = addrLen;
                    memcpy(&session.addr.st, &addr, addrLen);

                    dtls_handle_message(dtlsH, &session, buffer, numBytes);
#else
                    connP = connection_find(connList, &addr, addrLen);
                    if (connP == NULL)
                    {
                        connP = connection_new_incoming(connList, data.sock, (struct sockaddr *)&addr, addrLen);
                        if (connP != NULL)
                        {
                            connList = connP;
                        }
                    }
                    if (connP != NULL)
                    {
                        lwm2m_handle_packet(data.lwm2mH, buffer, numBytes, connP);
                    }
#endif
                }

                if (g_ipso == true)
                {
                    prv_ipso_display(&data);
                }
            }
            else if (FD_ISSET(STDIN_FILENO, &readfds))
            {
                numBytes = read(STDIN_FILENO, buffer, MAX_PACKET_SIZE - 1);

                if (numBytes > 1)
                {
                    buffer[numBytes] = 0;
                    handle_command(commands, (char*)buffer);
                }
                if (g_quit == 0)
                {
                    fprintf(stdout, "\r\n> ");
                    fflush(stdout);
                }
                else
                {
                    fprintf(stdout, "\r\n");
                }
            }
        }
    }

    lwm2m_close(data.lwm2mH);
    close(data.sock);
#ifdef WITH_TINYDTLS
    dtls_free_context(dtlsH);
#else
    connection_free(connList);
#endif

    return 0;
}
