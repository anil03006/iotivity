/* ****************************************************************
 *
 * Copyright 2015 Samsung Electronics All Rights Reserved.
 *
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ******************************************************************/

#include "iotivity_config.h"
#include "iotivity_debug.h"
#include <sys/types.h>
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_WS2TCPIP_H
#include <ws2tcpip.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif
#ifdef HAVE_SYS_POLL_H
#include <sys/poll.h>
#endif
#include <stdio.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <fcntl.h>
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_NET_IF_H
#include <net/if.h>
#endif
#include <errno.h>
#include <assert.h>

#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif

#include "catcpinterface.h"
#include "caipnwmonitor.h"
#include <coap/pdu.h>
#include "caadapterutils.h"
#include "octhread.h"
#include "oic_malloc.h"
#include "oic_string.h"

#ifdef __WITH_TLS__
#include "ca_adapter_net_ssl.h"
#endif

/**
 * Logging tag for module name.
 */
#define TAG "OIC_CA_TCP_SERVER"

/**
 * Maximum CoAP over TCP header length
 * to know the total data length.
 */
#define COAP_MAX_HEADER_SIZE  6

/**
 * TLS header size
 */
#define TLS_HEADER_SIZE 5

/**
 * Mutex to synchronize device object list.
 */
static oc_mutex g_mutexObjectList = NULL;

/**
 * Conditional mutex to synchronize.
 */
static oc_cond g_condObjectList = NULL;

/**
 * Maintains the callback to be notified when data received from remote device.
 */
static CATCPPacketReceivedCallback g_packetReceivedCallback = NULL;

/**
 * Error callback to update error in TCP.
 */
static CATCPErrorHandleCallback g_tcpErrorHandler = NULL;

/**
 * Connected Callback to pass the connection information to RI.
 */
static CATCPConnectionHandleCallback g_connectionCallback = NULL;

static CAResult_t CATCPCreateMutex();
static void CATCPDestroyMutex();
static CAResult_t CATCPCreateCond();
static void CATCPDestroyCond();
static CASocketFd_t CACreateAcceptSocket(int family, CASocket_t *sock);
static void CAAcceptConnection(CATransportFlags_t flag, CASocket_t *sock);
static void CAFindReadyMessage();
#if !defined(WSA_WAIT_EVENT_0)
static void CASelectReturned(fd_set *readFds);
#else
static void CASocketEventReturned(CASocketFd_t socket);
#endif
static void CAReceiveMessage(CASocketFd_t fd);
static void CAReceiveHandler(void *data);
static CAResult_t CATCPCreateSocket(int family, CATCPSessionInfo_t *svritem);

#if defined(WSA_WAIT_EVENT_0)
#define CHECKFD(FD)
#else
#define CHECKFD(FD) \
    if (FD > caglobals.tcp.maxfd) \
        caglobals.tcp.maxfd = FD;
#endif

#define CLOSE_SOCKET(TYPE) \
    if (caglobals.tcp.TYPE.fd != OC_INVALID_SOCKET) \
    { \
        OC_CLOSE_SOCKET(caglobals.tcp.TYPE.fd); \
        caglobals.tcp.TYPE.fd = OC_INVALID_SOCKET; \
    }

#define CA_FD_SET(TYPE, FDS) \
    if (caglobals.tcp.TYPE.fd != OC_INVALID_SOCKET) \
    { \
        FD_SET(caglobals.tcp.TYPE.fd, FDS); \
    }

static void CATCPDestroyMutex()
{
    if (g_mutexObjectList)
    {
        oc_mutex_free(g_mutexObjectList);
        g_mutexObjectList = NULL;
    }
}

static CAResult_t CATCPCreateMutex()
{
    if (!g_mutexObjectList)
    {
        g_mutexObjectList = oc_mutex_new();
        if (!g_mutexObjectList)
        {
            OIC_LOG(ERROR, TAG, "Failed to created mutex!");
            return CA_STATUS_FAILED;
        }
    }

    return CA_STATUS_OK;
}

static void CATCPDestroyCond()
{
    if (g_condObjectList)
    {
        oc_cond_free(g_condObjectList);
        g_condObjectList = NULL;
    }
}

static CAResult_t CATCPCreateCond()
{
    if (!g_condObjectList)
    {
        g_condObjectList = oc_cond_new();
        if (!g_condObjectList)
        {
            OIC_LOG(ERROR, TAG, "Failed to created cond!");
            return CA_STATUS_FAILED;
        }
    }
    return CA_STATUS_OK;
}

static void CAReceiveHandler(void *data)
{
    (void)data;
    OIC_LOG(DEBUG, TAG, "IN - CAReceiveHandler");

    while (!caglobals.tcp.terminate)
    {
        CAFindReadyMessage();
    }

    oc_mutex_lock(g_mutexObjectList);
    oc_cond_signal(g_condObjectList);
    oc_mutex_unlock(g_mutexObjectList);

    OIC_LOG(DEBUG, TAG, "OUT - CAReceiveHandler");
}

#if !defined(WSA_WAIT_EVENT_0)

static void CAFindReadyMessage()
{
    fd_set readFds;
    struct timeval timeout = { .tv_sec = caglobals.tcp.selectTimeout };

    FD_ZERO(&readFds);
    CA_FD_SET(ipv4, &readFds);
    CA_FD_SET(ipv4s, &readFds);
    CA_FD_SET(ipv6, &readFds);
    CA_FD_SET(ipv6s, &readFds);

    if (OC_INVALID_SOCKET != caglobals.tcp.shutdownFds[0])
    {
        FD_SET(caglobals.tcp.shutdownFds[0], &readFds);
    }
    if (OC_INVALID_SOCKET != caglobals.tcp.connectionFds[0])
    {
        FD_SET(caglobals.tcp.connectionFds[0], &readFds);
    }

    uint32_t length = u_arraylist_length(caglobals.tcp.svrlist);
    for (size_t i = 0; i < length; i++)
    {
        CATCPSessionInfo_t *svritem =
                (CATCPSessionInfo_t *) u_arraylist_get(caglobals.tcp.svrlist, i);
        if (svritem && 0 <= svritem->fd && CONNECTED == svritem->state)
        {
            FD_SET(svritem->fd, &readFds);
        }
    }

    int ret = select(caglobals.tcp.maxfd + 1, &readFds, NULL, NULL, &timeout);

    if (caglobals.tcp.terminate)
    {
        OIC_LOG_V(DEBUG, TAG, "Packet receiver Stop request received.");
        return;
    }

    if (0 == ret)
    {
        return;
    }
    else if (0 < ret)
    {
        CASelectReturned(&readFds);
    }
    else // if (0 > ret)
    {
        OIC_LOG_V(FATAL, TAG, "select error %s", strerror(errno));
        return;
    }
}

static void CASelectReturned(fd_set *readFds)
{
    VERIFY_NON_NULL_VOID(readFds, TAG, "readFds is NULL");

    if (caglobals.tcp.ipv4.fd != -1 && FD_ISSET(caglobals.tcp.ipv4.fd, readFds))
    {
        CAAcceptConnection(CA_IPV4, &caglobals.tcp.ipv4);
        return;
    }
    else if (caglobals.tcp.ipv4s.fd != -1 && FD_ISSET(caglobals.tcp.ipv4s.fd, readFds))
    {
        CAAcceptConnection(CA_IPV4 | CA_SECURE, &caglobals.tcp.ipv4s);
        return;
    }
    else if (caglobals.tcp.ipv6.fd != -1 && FD_ISSET(caglobals.tcp.ipv6.fd, readFds))
    {
        CAAcceptConnection(CA_IPV6, &caglobals.tcp.ipv6);
        return;
    }
    else if (caglobals.tcp.ipv6s.fd != -1 && FD_ISSET(caglobals.tcp.ipv6s.fd, readFds))
    {
        CAAcceptConnection(CA_IPV6 | CA_SECURE, &caglobals.tcp.ipv6s);
        return;
    }
    else if (-1 != caglobals.tcp.connectionFds[0] &&
            FD_ISSET(caglobals.tcp.connectionFds[0], readFds))
    {
        // new connection was created from remote device.
        // exit the function to update read file descriptor.
        char buf[MAX_ADDR_STR_SIZE_CA] = {0};
        ssize_t len = read(caglobals.tcp.connectionFds[0], buf, sizeof (buf));
        if (-1 == len)
        {
            return;
        }
        OIC_LOG_V(DEBUG, TAG, "Received new connection event with [%s]", buf);
        return;
    }
    else
    {
        uint32_t length = u_arraylist_length(caglobals.tcp.svrlist);
        for (size_t i = 0; i < length; i++)
        {
            CATCPSessionInfo_t *svritem =
                    (CATCPSessionInfo_t *) u_arraylist_get(caglobals.tcp.svrlist, i);
            if (svritem && svritem->fd >= 0)
            {
                if (FD_ISSET(svritem->fd, readFds))
                {
                    CAReceiveMessage(svritem->fd);
                }
            }
        }
    }
}

#else // if defined(WSA_WAIT_EVENT_0)

/**
 * Push an exiting socket event to listen on
 *
 * @param[in] s              Socket to push
 * @param[in] socketArray    Array in which to add socket
 * @param[in] event          Event to push
 * @param[in] eventArray     Array in which to add event
 * @param[in/out] eventIndex Current length of arrays
 * @param[in] arraySize      Maximum length of arrays
 * @return true on success, false on failure
 */
static bool CAPushEvent(CASocketFd_t s, CASocketFd_t* socketArray,
                        HANDLE event, HANDLE* eventArray, int* eventIndex, int arraySize)
{
    if (*eventIndex == arraySize)
    {
        return false;
    }

    assert(*eventIndex >= 0);
    socketArray[*eventIndex] = s;
    eventArray[(*eventIndex)++] = event;
    return true;
}

/**
 * Push a new socket event to listen on
 *
 * @param[in] s              Socket to push
 * @param[in] socketArray    Array in which to add socket
 * @param[in] eventArray     Array in which to add event
 * @param[in/out] eventIndex Current length of arrays
 * @param[in] arraySize      Maximum length of arrays
 * @return true on success, false on failure
 */
static bool CAPushSocket(CASocketFd_t s, CASocketFd_t* socketArray,
                         HANDLE *eventArray, int *eventIndex, int arraySize)
{
    if (s == OC_INVALID_SOCKET)
    {
        // Nothing to push.
        return true;
    }

    WSAEVENT newEvent = WSACreateEvent();
    if (WSA_INVALID_EVENT == newEvent)
    {
        OIC_LOG_V(ERROR, TAG, "WSACreateEvent(NewEvent) failed %u", WSAGetLastError());
        return false;
    }

    if (0 != WSAEventSelect(s, newEvent, FD_READ))
    {
        OIC_LOG_V(ERROR, TAG, "WSAEventSelect failed %u", WSAGetLastError());
        OC_VERIFY(WSACloseEvent(newEvent));
        return false;
    }

    if (!CAPushEvent(s, socketArray, newEvent, eventArray, eventIndex, arraySize))
    {
        OIC_LOG_V(ERROR, TAG, "CAPushEvent failed");
        OC_VERIFY(WSACloseEvent(newEvent));
        return false;
    }

    return true;
}

#define EVENT_ARRAY_SIZE 64

/**
 * Process any message that is ready
 */
static void CAFindReadyMessage()
{
    CASocketFd_t socketArray[EVENT_ARRAY_SIZE] = {0};
    HANDLE eventArray[_countof(socketArray)];
    int arraySize = 0;
    int eventIndex;

    if (OC_INVALID_SOCKET != caglobals.tcp.ipv4.fd)
    {
        CAPushSocket(caglobals.tcp.ipv4.fd, socketArray, eventArray, &arraySize, _countof(socketArray));
    }
    if (OC_INVALID_SOCKET != caglobals.tcp.ipv6.fd)
    {
        CAPushSocket(caglobals.tcp.ipv6.fd, socketArray, eventArray, &arraySize, _countof(socketArray));
    }
    if (WSA_INVALID_EVENT != caglobals.tcp.shutdownEvent)
    {
        CAPushEvent(OC_INVALID_SOCKET, socketArray,
                    caglobals.tcp.shutdownEvent, eventArray, &arraySize, _countof(socketArray));
    }

    uint32_t length = u_arraylist_length(caglobals.tcp.svrlist);
    for (size_t i = 0; (i < length) && (arraySize < EVENT_ARRAY_SIZE); i++)
    {
        CATCPSessionInfo_t *svritem =
                (CATCPSessionInfo_t *) u_arraylist_get(caglobals.tcp.svrlist, i);
        if (svritem && OC_INVALID_SOCKET != svritem->fd)
        {
            CAPushSocket(svritem->fd, socketArray, eventArray, &arraySize, _countof(socketArray));
        }
    }

    // Should not have overflowed buffer
    assert(arraySize <= (_countof(socketArray)));

    while (!caglobals.tcp.terminate)
    {
        DWORD ret = WSAWaitForMultipleEvents(arraySize, eventArray, FALSE, WSA_INFINITE, FALSE);
        eventIndex = ret - WSA_WAIT_EVENT_0;
        assert((eventIndex >= 0) && (eventIndex < arraySize));

        if (false == WSAResetEvent(eventArray[eventIndex]))
        {
            OIC_LOG_V(ERROR, TAG, "WSAResetEvent failed %u", WSAGetLastError());
        }

        if ((caglobals.tcp.shutdownEvent != WSA_INVALID_EVENT) &&
            (caglobals.tcp.shutdownEvent == eventArray[eventIndex]))
        {
            continue;
        }

        // All other events are socket events.
        CASocketEventReturned(socketArray[eventIndex]);
    }

    while (arraySize > 0)
    {
        arraySize--;
        OC_VERIFY(WSACloseEvent(eventArray[arraySize]));
    }

    if (caglobals.tcp.terminate)
    {
        caglobals.tcp.shutdownEvent = WSA_INVALID_EVENT;
        WSACleanup();
    }
}

/**
 * Process an event (accept or receive) that is ready on a socket
 *
 * @param[in] s Socket to process
 */
static void CASocketEventReturned(CASocketFd_t s)
{
    if (caglobals.tcp.terminate)
    {
        return;
    }

    if ((caglobals.tcp.ipv4.fd != OC_INVALID_SOCKET) && (caglobals.tcp.ipv4.fd == s))
    {
        CAAcceptConnection(CA_IPV4, &caglobals.tcp.ipv4);
        return;
    }
    if ((caglobals.tcp.ipv6.fd != OC_INVALID_SOCKET) && (caglobals.tcp.ipv6.fd == s))
    {
        CAAcceptConnection(CA_IPV6, &caglobals.tcp.ipv6);
        return;
    }

    CAReceiveMessage(s);
}

#endif // WSA_WAIT_EVENT_0

static void CAAcceptConnection(CATransportFlags_t flag, CASocket_t *sock)
{
    VERIFY_NON_NULL_VOID(sock, TAG, "sock is NULL");

    struct sockaddr_storage clientaddr;
    socklen_t clientlen = sizeof (struct sockaddr_in);
    if (flag & CA_IPV6)
    {
        clientlen = sizeof(struct sockaddr_in6);
    }

    CASocketFd_t sockfd = accept(sock->fd, (struct sockaddr *)&clientaddr, &clientlen);
    if (OC_INVALID_SOCKET != sockfd)
    {
        CATCPSessionInfo_t *svritem =
                (CATCPSessionInfo_t *) OICCalloc(1, sizeof (*svritem));
        if (!svritem)
        {
            OIC_LOG(ERROR, TAG, "Out of memory");
            OC_CLOSE_SOCKET(sockfd);
            return;
        }

        svritem->fd = sockfd;
        svritem->sep.endpoint.flags = flag;
        svritem->sep.endpoint.adapter = CA_ADAPTER_TCP;
        svritem->state = CONNECTED;
        svritem->isClient = false;
        CAConvertAddrToName((struct sockaddr_storage *)&clientaddr, clientlen,
                            svritem->sep.endpoint.addr, &svritem->sep.endpoint.port);

        oc_mutex_lock(g_mutexObjectList);
        bool result = u_arraylist_add(caglobals.tcp.svrlist, svritem);
        if (!result)
        {
            OIC_LOG(ERROR, TAG, "u_arraylist_add failed.");
            OC_CLOSE_SOCKET(sockfd);
            OICFree(svritem);
            oc_mutex_unlock(g_mutexObjectList);
            return;
        }
        oc_mutex_unlock(g_mutexObjectList);

        CHECKFD(sockfd);

        // pass the connection information to CA Common Layer.
        if (g_connectionCallback)
        {
            g_connectionCallback(&(svritem->sep.endpoint), true, svritem->isClient);
        }
    }
}

#ifdef __WITH_TLS__
static bool CAIsTlsMessage(const unsigned char* data, size_t length)
{
    if (NULL == data || 0 == length)
    {
        OIC_LOG_V(ERROR, TAG, "%s: null input param", __func__);
        return false;
    }

    unsigned char first_byte = data[0];

    //TLS Plaintext has four types: change_cipher_spec = [14], alert = [15],
    //handshake = [16], application_data = [17] in HEX
    const uint8_t tls_head_type[] = {0x14, 0x15, 0x16, 0x17};
    size_t i = 0;

    for (i = 0; i < sizeof(tls_head_type); i++)
    {
        if(tls_head_type[i] == first_byte)
        {
            return true;
        }
    }

    return false;
}
#endif

/**
 * Clean socket state data
 *
 * @param[in/out] item - socket state data
 */
void CACleanData(CATCPSessionInfo_t *svritem)
{
    if (svritem)
    {
        OICFree(svritem->data);
        svritem->data = NULL;
        svritem->len = 0;
        svritem->tlsLen = 0;
        svritem->totalLen = 0;
        svritem->protocol = UNKNOWN;
    }
}

/**
 * Construct CoAP header and payload from buffer
 *
 * @param[in] svritem - used socket, buffer, current received message length and protocol
 * @param[in/out]  data  - data buffer, this value is updated as data is copied to svritem
 * @param[in/out]  dataLength  - length of data, this value decreased as data is copied to svritem
 * @return             - CA_STATUS_OK or appropriate error code
 */
CAResult_t CAConstructCoAP(CATCPSessionInfo_t *svritem, unsigned char **data,
                          size_t *dataLength)
{
    OIC_LOG_V(DEBUG, TAG, "In %s", __func__);

    if (NULL == svritem || NULL == data || NULL == dataLength)
    {
        OIC_LOG(ERROR, TAG, "Invalid input parameter(NULL)");
        return CA_STATUS_INVALID_PARAM;
    }

    unsigned char *inBuffer = *data;
    size_t inLen = *dataLength;
    OIC_LOG_V(DEBUG, TAG, "before-datalength : %u", *dataLength);

    if (NULL == svritem->data && inLen > 0)
    {
        // allocate memory for message header (CoAP header size because it is bigger)
        svritem->data = (unsigned char *) OICCalloc(1, COAP_MAX_HEADER_SIZE);
        if (NULL == svritem->data)
        {
            OIC_LOG(ERROR, TAG, "OICCalloc - out of memory");
            return CA_MEMORY_ALLOC_FAILED;
        }

        // copy 1 byte to parse coap header length
        memcpy(svritem->data, inBuffer, 1);
        svritem->len = 1;
        inBuffer++;
        inLen--;
    }

    //if not enough data received - read them on next CAFillHeader() call
    if (0 == inLen)
    {
        return CA_STATUS_OK;
    }

    //if enough data received - parse header
    svritem->protocol = COAP;

    //seems CoAP data received. read full coap header.
    coap_transport_t transport = coap_get_tcp_header_type_from_initbyte(svritem->data[0] >> 4);
    size_t headerLen = coap_get_tcp_header_length_for_transport(transport);
    size_t copyLen = 0;

    // HEADER
    if (svritem->len < headerLen)
    {
        copyLen = headerLen - svritem->len;
        if (inLen < copyLen)
        {
            copyLen = inLen;
        }

        //read required bytes to have full CoAP header
        memcpy(svritem->data + svritem->len, inBuffer, copyLen);
        svritem->len += copyLen;
        inBuffer += copyLen;
        inLen -= copyLen;

        //if not enough data received - read them on next CAFillHeader() call
        if (svritem->len < headerLen)
        {
            *data = inBuffer;
            *dataLength = inLen;
            OIC_LOG(DEBUG, TAG, "CoAP header received partially. Wait for rest header data");
            return CA_STATUS_OK;
        }

        //calculate CoAP message length
        svritem->totalLen = CAGetTotalLengthFromHeader(svritem->data);

        // allocate required memory
        unsigned char *buffer = OICRealloc(svritem->data, svritem->totalLen);
        if (NULL == buffer)
        {
            OIC_LOG(ERROR, TAG, "OICRealloc - out of memory");
            return CA_MEMORY_ALLOC_FAILED;
        }
        svritem->data = buffer;
    }

    // PAYLOAD
    if (inLen > 0)
    {
        // read required bytes to have full CoAP payload
        copyLen = svritem->totalLen - svritem->len;
        if (inLen < copyLen)
        {
            copyLen = inLen;
        }

        //read required bytes to have full CoAP header
        memcpy(svritem->data + svritem->len, inBuffer, copyLen);
        svritem->len += copyLen;
        inBuffer += copyLen;
        inLen -= copyLen;
    }

    *data = inBuffer;
    *dataLength = inLen;

    OIC_LOG_V(DEBUG, TAG, "after-datalength : %u", *dataLength);
    OIC_LOG_V(DEBUG, TAG, "Out %s", __func__);
    return CA_STATUS_OK;
}

static void CAReceiveMessage(CASocketFd_t fd)
{
    CAResult_t res = CA_STATUS_OK;

    //get remote device information from file descriptor.
    size_t index = 0;
    CATCPSessionInfo_t *svritem = CAGetSessionInfoFromFD(fd, &index);
    if (!svritem)
    {
        OIC_LOG(ERROR, TAG, "there is no connection information in list");
        return;
    }

    // read data
    int len = 0;

    if (svritem->sep.endpoint.flags & CA_SECURE)
    {
        svritem->protocol = TLS;

#ifdef __WITH_TLS__
        size_t nbRead = 0;
        size_t tlsLength = 0;

        if (TLS_HEADER_SIZE > svritem->tlsLen)
        {
            nbRead = TLS_HEADER_SIZE - svritem->tlsLen;
        }
        else
        {
            //[3][4] bytes in tls header are tls payload length
            tlsLength = TLS_HEADER_SIZE +
                            (size_t)((svritem->tlsdata[3] << 8) | svritem->tlsdata[4]);
            OIC_LOG_V(DEBUG, TAG, "toal tls length = %u", tlsLength);
            if (tlsLength > sizeof(svritem->tlsdata))
            {
                OIC_LOG_V(ERROR, TAG, "toal tls length is too big (buffer size : %u)",
                                    sizeof(svritem->tlsdata));
                return;
            }
            nbRead = tlsLength - svritem->tlsLen;
        }

        len = recv(fd, svritem->tlsdata + svritem->tlsLen, nbRead, 0);
        if (len < 0)
        {
            OIC_LOG_V(ERROR, TAG, "recv failed %s", strerror(errno));
            res = CA_RECEIVE_FAILED;
        }
        else if (0 == len)
        {
            OIC_LOG(INFO, TAG, "Received disconnect from peer. Close connection");
            res = CA_DESTINATION_DISCONNECTED;
        }
        else
        {
            svritem->tlsLen += len;
            OIC_LOG_V(DEBUG, TAG, "nb_read : %u bytes , recv() : %d bytes, svritem->tlsLen : %u bytes",
                                nbRead, len, svritem->tlsLen);
            if (tlsLength > 0 && tlsLength == svritem->tlsLen)
            {
                //when successfully read data - pass them to callback.
                res = CAdecryptSsl(&svritem->sep, (uint8_t *)svritem->tlsdata, svritem->tlsLen);
                svritem->tlsLen = 0;
                OIC_LOG_V(DEBUG, TAG, "%s: CAdecryptSsl returned %d", __func__, res);
            }
        }
#endif

    }
    else
    {
        svritem->protocol = COAP;

        // svritem->tlsdata can also be used as receiving buffer in case of raw tcp
        len = recv(fd, svritem->tlsdata, sizeof(svritem->tlsdata), 0);
        if (len < 0)
        {
            OIC_LOG_V(ERROR, TAG, "recv failed %s", strerror(errno));
            res = CA_RECEIVE_FAILED;
        }
        else if (0 == len)
        {
            OIC_LOG(INFO, TAG, "Received disconnect from peer. Close connection");
            res = CA_DESTINATION_DISCONNECTED;
        }
        else
        {
            OIC_LOG_V(DEBUG, TAG, "recv() : %d bytes", len);
            //when successfully read data - pass them to callback.
            if (g_packetReceivedCallback)
            {
                g_packetReceivedCallback(&svritem->sep, svritem->tlsdata, len);
            }
        }
    }

    //disconnect session and clean-up data if any error occurs
    if (res != CA_STATUS_OK)
    {
#ifdef __WITH_TLS__
        if (CA_STATUS_OK != CAcloseSslConnection(&svritem->sep.endpoint))
        {
            OIC_LOG(ERROR, TAG, "Failed to close TLS session");
        }
#endif
        CASearchAndDeleteTCPSession(&(svritem->sep.endpoint));
        return;
    }
}

#if !defined(WSA_WAIT_EVENT_0)
static ssize_t CAWakeUpForReadFdsUpdate(const char *host)
{
    if (caglobals.tcp.connectionFds[1] != -1)
    {
        ssize_t len = 0;
        do
        {
            len = write(caglobals.tcp.connectionFds[1], host, strlen(host));
        } while ((len == -1) && (errno == EINTR));

        if ((len == -1) && (errno != EINTR) && (errno != EPIPE))
        {
            OIC_LOG_V(DEBUG, TAG, "write failed: %s", strerror(errno));
        }
        return len;
    }
    return -1;
}
#endif

static CAResult_t CATCPConvertNameToAddr(int family, const char *host, uint16_t port,
                                         struct sockaddr_storage *sockaddr)
{
    struct addrinfo *addrs = NULL;
    struct addrinfo hints = { .ai_family = family,
                              .ai_protocol   = IPPROTO_TCP,
                              .ai_socktype = SOCK_STREAM,
                              .ai_flags = AI_NUMERICHOST };

    int r = getaddrinfo(host, NULL, &hints, &addrs);
    if (r)
    {
#ifdef EAI_SYSTEM
        if (EAI_SYSTEM == r)
        {
            OIC_LOG_V(ERROR, TAG, "getaddrinfo failed: errno %s", strerror(errno));
        }
        else
#endif
        {
            OIC_LOG_V(ERROR, TAG, "getaddrinfo failed: %s", gai_strerror(r));
        }
        freeaddrinfo(addrs);
        return CA_STATUS_FAILED;
    }
    // assumption: in this case, getaddrinfo will only return one addrinfo
    // or first is the one we want.
    if (addrs[0].ai_family == AF_INET6)
    {
        memcpy(sockaddr, addrs[0].ai_addr, sizeof (struct sockaddr_in6));
        ((struct sockaddr_in6 *)sockaddr)->sin6_port = htons(port);
    }
    else
    {
        memcpy(sockaddr, addrs[0].ai_addr, sizeof (struct sockaddr_in));
        ((struct sockaddr_in *)sockaddr)->sin_port = htons(port);
    }
    freeaddrinfo(addrs);
    return CA_STATUS_OK;
}

static CAResult_t CATCPCreateSocket(int family, CATCPSessionInfo_t *svritem)
{
    VERIFY_NON_NULL(svritem, TAG, "svritem is NULL");

    OIC_LOG_V(DEBUG, TAG, "try to connect with [%s:%u]",
              svritem->sep.endpoint.addr, svritem->sep.endpoint.port);

    // #1. create tcp socket.
    CASocketFd_t fd = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (OC_INVALID_SOCKET == fd)
    {
        OIC_LOG_V(ERROR, TAG, "create socket failed: %s", strerror(errno));
        return CA_SOCKET_OPERATION_FAILED;
    }
    svritem->fd = fd;

    // #2. convert address from string to binary.
    struct sockaddr_storage sa = { .ss_family = family };
    CAResult_t res = CAConvertNameToAddr(svritem->sep.endpoint.addr,
                                         svritem->sep.endpoint.port, &sa);
    if (CA_STATUS_OK != res)
    {
        OIC_LOG(ERROR, TAG, "convert name to sockaddr failed");
        return CA_SOCKET_OPERATION_FAILED;
    }

    // #3. set socket length.
    socklen_t socklen = 0;
    if (sa.ss_family == AF_INET6)
    {
        socklen = sizeof(struct sockaddr_in6);
    }
    else
    {
        socklen = sizeof(struct sockaddr_in);
    }

    // #4. connect to remote server device.
    if (connect(fd, (struct sockaddr *)&sa, socklen) < 0)
    {
        OIC_LOG_V(ERROR, TAG, "failed to connect socket, %s", strerror(errno));
        CALogSendStateInfo(svritem->sep.endpoint.adapter, svritem->sep.endpoint.addr,
                           svritem->sep.endpoint.port, 0, false, strerror(errno));
        return CA_SOCKET_OPERATION_FAILED;
    }

    OIC_LOG(DEBUG, TAG, "connect socket success");
    svritem->state = CONNECTED;
    CHECKFD(svritem->fd);
#if !defined(WSA_WAIT_EVENT_0)
    ssize_t len = CAWakeUpForReadFdsUpdate(svritem->sep.endpoint.addr);
    if (-1 == len)
    {
        OIC_LOG(ERROR, TAG, "wakeup receive thread failed");
        return CA_SOCKET_OPERATION_FAILED;
    }
#endif
    return CA_STATUS_OK;
}

static CASocketFd_t CACreateAcceptSocket(int family, CASocket_t *sock)
{
    VERIFY_NON_NULL_RET(sock, TAG, "sock", OC_INVALID_SOCKET);

    if (OC_INVALID_SOCKET != sock->fd)
    {
        OIC_LOG(DEBUG, TAG, "accept socket created already");
        return sock->fd;
    }

    socklen_t socklen = 0;
    struct sockaddr_storage server = { .ss_family = family };

    CASocketFd_t fd = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (OC_INVALID_SOCKET == fd)
    {
        OIC_LOG(ERROR, TAG, "Failed to create socket");
        goto exit;
    }

    if (family == AF_INET6)
    {
        // the socket is restricted to sending and receiving IPv6 packets only.
        int on = 1;
        if (OC_SOCKET_ERROR == setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, OPTVAL_T(&on), sizeof (on)))
        {
            OIC_LOG_V(ERROR, TAG, "IPV6_V6ONLY failed: %s", strerror(errno));
            goto exit;
        }
        ((struct sockaddr_in6 *)&server)->sin6_port = htons(sock->port);
        socklen = sizeof (struct sockaddr_in6);
    }
    else
    {
        ((struct sockaddr_in *)&server)->sin_port = htons(sock->port);
        socklen = sizeof (struct sockaddr_in);
    }

    int reuse = 1;
    if (OC_SOCKET_ERROR == setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, OPTVAL_T(&reuse), sizeof(reuse)))
    {
        OIC_LOG(ERROR, TAG, "setsockopt SO_REUSEADDR");
        goto exit;
    }

    if (OC_SOCKET_ERROR == bind(fd, (struct sockaddr *)&server, socklen))
    {
        OIC_LOG_V(ERROR, TAG, "bind socket failed: %s", strerror(errno));
        goto exit;
    }

    if (listen(fd, caglobals.tcp.listenBacklog) != 0)
    {
        OIC_LOG(ERROR, TAG, "listen() error");
        goto exit;
    }

    if (!sock->port)  // return the assigned port
    {
        if (OC_SOCKET_ERROR == getsockname(fd, (struct sockaddr *)&server, &socklen))
        {
            OIC_LOG_V(ERROR, TAG, "getsockname failed: %s", strerror(errno));
            goto exit;
        }
        sock->port = ntohs(family == AF_INET6 ?
                      ((struct sockaddr_in6 *)&server)->sin6_port :
                      ((struct sockaddr_in *)&server)->sin_port);
    }

    return fd;

exit:
    if (fd != OC_INVALID_SOCKET)
    {
        OC_CLOSE_SOCKET(fd);
    }
    return OC_INVALID_SOCKET;
}

#if !defined(WSA_WAIT_EVENT_0)
static void CAInitializePipe(int *fds)
{
    int ret = pipe(fds);
    if (-1 != ret)
    {
        ret = fcntl(fds[0], F_GETFD);
        if (-1 != ret)
        {
            ret = fcntl(fds[0], F_SETFD, ret|FD_CLOEXEC);
        }
        if (-1 != ret)
        {
            ret = fcntl(fds[1], F_GETFD);
        }
        if (-1 != ret)
        {
            ret = fcntl(fds[1], F_SETFD, ret|FD_CLOEXEC);
        }
        if (-1 == ret)
        {
            close(fds[1]);
            close(fds[0]);

            fds[0] = -1;
            fds[1] = -1;

            OIC_LOG_V(ERROR, TAG, "pipe failed: %s", strerror(errno));
        }
    }
}
#endif

#define NEWSOCKET(FAMILY, NAME) \
    caglobals.tcp.NAME.fd = CACreateAcceptSocket(FAMILY, &caglobals.tcp.NAME); \
    if (caglobals.tcp.NAME.fd == OC_INVALID_SOCKET) \
    { \
        caglobals.tcp.NAME.port = 0; \
        caglobals.tcp.NAME.fd = CACreateAcceptSocket(FAMILY, &caglobals.tcp.NAME); \
    } \
    CHECKFD(caglobals.tcp.NAME.fd);

CAResult_t CATCPStartServer(const ca_thread_pool_t threadPool)
{
    if (caglobals.tcp.started)
    {
        OIC_LOG(DEBUG, TAG, "Adapter is started already");
        return CA_STATUS_OK;
    }

    if (!caglobals.tcp.ipv4tcpenabled)
    {
        caglobals.tcp.ipv4tcpenabled = true;    // only needed to run CA tests
    }
    if (!caglobals.tcp.ipv6tcpenabled)
    {
        caglobals.tcp.ipv6tcpenabled = true;    // only needed to run CA tests
    }

    CAResult_t res = CATCPCreateMutex();
    if (CA_STATUS_OK == res)
    {
        res = CATCPCreateCond();
    }
    if (CA_STATUS_OK != res)
    {
        OIC_LOG(ERROR, TAG, "failed to create mutex/cond");
        return res;
    }

    oc_mutex_lock(g_mutexObjectList);
    if (!caglobals.tcp.svrlist)
    {
        caglobals.tcp.svrlist = u_arraylist_create();
    }
    oc_mutex_unlock(g_mutexObjectList);

    if (caglobals.server)
    {
        NEWSOCKET(AF_INET, ipv4);
        NEWSOCKET(AF_INET, ipv4s);
        NEWSOCKET(AF_INET6, ipv6);
        NEWSOCKET(AF_INET6, ipv6s);
        OIC_LOG_V(DEBUG, TAG, "IPv4 socket fd=%d, port=%d",
                  caglobals.tcp.ipv4.fd, caglobals.tcp.ipv4.port);
        OIC_LOG_V(DEBUG, TAG, "IPv4 secure socket fd=%d, port=%d",
                  caglobals.tcp.ipv4s.fd, caglobals.tcp.ipv4s.port);
        OIC_LOG_V(DEBUG, TAG, "IPv6 socket fd=%d, port=%d",
                  caglobals.tcp.ipv6.fd, caglobals.tcp.ipv6.port);
        OIC_LOG_V(DEBUG, TAG, "IPv6 secure socket fd=%d, port=%d",
                  caglobals.tcp.ipv6s.fd, caglobals.tcp.ipv6s.port);
    }

    // create mechanism for fast shutdown
#ifdef WSA_WAIT_EVENT_0
    caglobals.tcp.shutdownEvent = WSACreateEvent();
    if (WSA_INVALID_EVENT == caglobals.tcp.shutdownEvent)
    {
        OIC_LOG(ERROR, TAG, "failed to create shutdown event");
        return res;
    }
#else
    CAInitializePipe(caglobals.tcp.shutdownFds);
    CHECKFD(caglobals.tcp.shutdownFds[0]);
    CHECKFD(caglobals.tcp.shutdownFds[1]);
#endif

#ifndef WSA_WAIT_EVENT_0
    CAInitializePipe(caglobals.tcp.connectionFds);
    CHECKFD(caglobals.tcp.connectionFds[0]);
    CHECKFD(caglobals.tcp.connectionFds[1]);
#endif

    caglobals.tcp.terminate = false;
    res = ca_thread_pool_add_task(threadPool, CAReceiveHandler, NULL);
    if (CA_STATUS_OK != res)
    {
        OIC_LOG(ERROR, TAG, "thread_pool_add_task failed");
        return res;
    }
    OIC_LOG(DEBUG, TAG, "CAReceiveHandler thread started successfully.");

    caglobals.tcp.started = true;
    return CA_STATUS_OK;
}

void CATCPStopServer()
{
    if (caglobals.tcp.terminate)
    {
        OIC_LOG(DEBUG, TAG, "Adapter is not enabled");
        return;
    }

    // mutex lock
    oc_mutex_lock(g_mutexObjectList);

    // set terminate flag.
    caglobals.tcp.terminate = true;

#if !defined(WSA_WAIT_EVENT_0)
    if (caglobals.tcp.shutdownFds[1] != -1)
    {
        close(caglobals.tcp.shutdownFds[1]);
        caglobals.tcp.shutdownFds[1] = OC_INVALID_SOCKET;
        // receive thread will stop immediately
    }
    if (caglobals.tcp.connectionFds[1] != -1)
    {
        close(caglobals.tcp.connectionFds[1]);
        caglobals.tcp.connectionFds[1] = OC_INVALID_SOCKET;
    }
#else
    // receive thread will stop immediately.
    if (!WSASetEvent(caglobals.tcp.shutdownEvent))
    {
        OIC_LOG_V(DEBUG, TAG, "set shutdown event failed: %u", GetLastError());
    }
#endif

    // close accept socket.
    CLOSE_SOCKET(ipv4);
    CLOSE_SOCKET(ipv4s);
    CLOSE_SOCKET(ipv6);
    CLOSE_SOCKET(ipv6s);

    if (caglobals.tcp.started)
    {
        oc_cond_wait(g_condObjectList, g_mutexObjectList);
    }
    caglobals.tcp.started = false;

    // mutex unlock
    oc_mutex_unlock(g_mutexObjectList);

    CATCPDisconnectAll();
    CATCPDestroyMutex();
    CATCPDestroyCond();

    OIC_LOG(DEBUG, TAG, "Adapter terminated successfully");
}

void CATCPSetPacketReceiveCallback(CATCPPacketReceivedCallback callback)
{
    g_packetReceivedCallback = callback;
}

void CATCPSetConnectionChangedCallback(CATCPConnectionHandleCallback connHandler)
{
    g_connectionCallback = connHandler;
}

size_t CACheckPayloadLengthFromHeader(const void *data, size_t dlen)
{
    VERIFY_NON_NULL_RET(data, TAG, "data", -1);

    coap_transport_t transport = coap_get_tcp_header_type_from_initbyte(
            ((unsigned char *)data)[0] >> 4);

    coap_pdu_t *pdu = coap_new_pdu2(transport, dlen);
    if (!pdu)
    {
        OIC_LOG(ERROR, TAG, "outpdu is null");
        return 0;
    }

    int ret = coap_pdu_parse2((unsigned char *) data, dlen, pdu, transport);
    if (0 >= ret)
    {
        OIC_LOG(ERROR, TAG, "pdu parse failed");
        coap_delete_pdu(pdu);
        return 0;
    }

    size_t payloadLen = 0;
    size_t headerSize = coap_get_tcp_header_length_for_transport(transport);
    OIC_LOG_V(DEBUG, TAG, "headerSize : %zu, pdu length : %d",
              headerSize, pdu->length);
    if (pdu->length > headerSize)
    {
        payloadLen = (unsigned char *) pdu->hdr + pdu->length - pdu->data;
    }

    OICFree(pdu);

    return payloadLen;
}

static ssize_t sendData(const CAEndpoint_t *endpoint, const void *data,
                        size_t dlen, const char *fam)
{
    OIC_LOG_V(INFO, TAG, "The length of data that needs to be sent is %zu bytes", dlen);

    // #1. find a session info from list.
    CASocketFd_t sockFd = CAGetSocketFDFromEndpoint(endpoint);
    if (OC_INVALID_SOCKET == sockFd)
    {
        // if there is no connection info, connect to remote device.
        sockFd = CAConnectTCPSession(endpoint);
        if (OC_INVALID_SOCKET == sockFd)
        {
            OIC_LOG(ERROR, TAG, "Failed to create tcp session object");
            return -1;
        }
    }

    // #2. send data to remote device.
    ssize_t remainLen = dlen;
    do
    {
        ssize_t len = send(sockFd, data, remainLen, 0);
        if (-1 == len)
        {
            if (EWOULDBLOCK != errno)
            {
                OIC_LOG_V(ERROR, TAG, "unicast ipv4tcp sendTo failed: %s", strerror(errno));
                CALogSendStateInfo(endpoint->adapter, endpoint->addr, endpoint->port,
                                   len, false, strerror(errno));
                return len;
            }
            continue;
        }
        data = ((char*)data) + len;
        remainLen -= len;
    } while (remainLen > 0);

#ifndef TB_LOG
    (void)fam;
#endif
    OIC_LOG_V(INFO, TAG, "unicast %stcp sendTo is successful: %zu bytes", fam, dlen);
    CALogSendStateInfo(endpoint->adapter, endpoint->addr, endpoint->port,
                       dlen, true, NULL);
    return dlen;
}

ssize_t CATCPSendData(CAEndpoint_t *endpoint, const void *data, size_t datalen)
{
    OIC_LOG_V(DEBUG, TAG, "%s", __func__);
    VERIFY_NON_NULL_RET(endpoint, TAG, "endpoint is NULL", -1);
    VERIFY_NON_NULL_RET(data, TAG, "data is NULL", -1);

    if (caglobals.tcp.ipv6tcpenabled && (endpoint->flags & CA_IPV6))
    {
        return sendData(endpoint, data, datalen, "ipv6");
    }
    if (caglobals.tcp.ipv4tcpenabled && (endpoint->flags & CA_IPV4))
    {
        return sendData(endpoint, data, datalen, "ipv4");
    }

    OIC_LOG(ERROR, TAG, "Not supported transport flags");
    return -1;
}

CAResult_t CAGetTCPInterfaceInformation(CAEndpoint_t **info, uint32_t *size)
{
    VERIFY_NON_NULL(info, TAG, "info is NULL");
    VERIFY_NON_NULL(size, TAG, "size is NULL");

    u_arraylist_t *iflist = CAIPGetInterfaceInformation(0);
    if (!iflist)
    {
        OIC_LOG_V(ERROR, TAG, "get interface info failed: %s", strerror(errno));
        return CA_STATUS_FAILED;
    }

    uint32_t len = u_arraylist_length(iflist);

    CAEndpoint_t *ep = (CAEndpoint_t *)OICCalloc(len, sizeof (CAEndpoint_t));
    if (!ep)
    {
        OIC_LOG(ERROR, TAG, "Malloc Failed");
        u_arraylist_destroy(iflist);
        return CA_MEMORY_ALLOC_FAILED;
    }

    for (uint32_t i = 0, j = 0; i < len; i++)
    {
        CAInterface_t *ifitem = (CAInterface_t *)u_arraylist_get(iflist, i);
        if (!ifitem)
        {
            continue;
        }

        ep[j].adapter = CA_ADAPTER_TCP;
        ep[j].ifindex = ifitem->index;

        if (ifitem->family == AF_INET6)
        {
            ep[j].flags = CA_IPV6;
            ep[j].port = caglobals.tcp.ipv6.port;
        }
        else if (ifitem->family == AF_INET)
        {
            ep[j].flags = CA_IPV4;
            ep[j].port = caglobals.tcp.ipv4.port;
        }
        else
        {
            continue;
        }
        OICStrcpy(ep[j].addr, sizeof(ep[j].addr), ifitem->addr);
        j++;
    }

    *info = ep;
    *size = len;

    u_arraylist_destroy(iflist);

    return CA_STATUS_OK;
}

CASocketFd_t CAConnectTCPSession(const CAEndpoint_t *endpoint)
{
    OIC_LOG_V(DEBUG, TAG, "%s", __func__);
    VERIFY_NON_NULL_RET(endpoint, TAG, "endpoint is NULL", OC_INVALID_SOCKET);

    // #1. create TCP server object
    CATCPSessionInfo_t *svritem = (CATCPSessionInfo_t *) OICCalloc(1, sizeof (*svritem));
    if (!svritem)
    {
        OIC_LOG(ERROR, TAG, "Out of memory");
        return OC_INVALID_SOCKET;
    }
    memcpy(svritem->sep.endpoint.addr, endpoint->addr, sizeof(svritem->sep.endpoint.addr));
    svritem->sep.endpoint.adapter = endpoint->adapter;
    svritem->sep.endpoint.port = endpoint->port;
    svritem->sep.endpoint.flags = endpoint->flags;
    svritem->sep.endpoint.ifindex = endpoint->ifindex;
    svritem->state = CONNECTING;
    svritem->isClient = true;

    // #2. add TCP connection info to list
    oc_mutex_lock(g_mutexObjectList);
    if (caglobals.tcp.svrlist)
    {
        bool res = u_arraylist_add(caglobals.tcp.svrlist, svritem);
        if (!res)
        {
            OIC_LOG(ERROR, TAG, "u_arraylist_add failed.");
            OC_CLOSE_SOCKET(svritem->fd);
            OICFree(svritem);
            oc_mutex_unlock(g_mutexObjectList);
            return OC_INVALID_SOCKET;
        }
    }
    oc_mutex_unlock(g_mutexObjectList);

    // #3. create the socket and connect to TCP server
    int family = (svritem->sep.endpoint.flags & CA_IPV6) ? AF_INET6 : AF_INET;
    if (CA_STATUS_OK != CATCPCreateSocket(family, svritem))
    {
        return OC_INVALID_SOCKET;
    }

    // #4. pass the connection information to CA Common Layer.
    if (g_connectionCallback)
    {
        g_connectionCallback(&(svritem->sep.endpoint), true, svritem->isClient);
    }

    return svritem->fd;
}

CAResult_t CADisconnectTCPSession(size_t index)
{
    OIC_LOG_V(DEBUG, TAG, "%s", __func__);

    CATCPSessionInfo_t *removedData = u_arraylist_remove(caglobals.tcp.svrlist, index);
    if (!removedData)
    {
        OIC_LOG(DEBUG, TAG, "there is no data to be removed");
        return CA_STATUS_OK;
    }

    // close the socket and remove session info in list.
    if (removedData->fd != OC_INVALID_SOCKET)
    {
        shutdown(removedData->fd, SHUT_RDWR);
        OC_CLOSE_SOCKET(removedData->fd);
        removedData->fd = OC_INVALID_SOCKET;
        OIC_LOG(DEBUG, TAG, "close socket");
        removedData->state = (CONNECTED == removedData->state) ?
                                    DISCONNECTED : removedData->state;

        // pass the connection information to CA Common Layer.
        if (g_connectionCallback && DISCONNECTED == removedData->state)
        {
            g_connectionCallback(&(removedData->sep.endpoint), false, removedData->isClient);
        }
    }
    OICFree(removedData->data);
    removedData->data = NULL;

    OICFree(removedData);
    removedData = NULL;

    OIC_LOG(DEBUG, TAG, "data is removed from session list");
    return CA_STATUS_OK;
}

void CATCPDisconnectAll()
{
    oc_mutex_lock(g_mutexObjectList);

    uint32_t length = u_arraylist_length(caglobals.tcp.svrlist);
    for (ssize_t index = length; index > 0; index--)
    {
        // disconnect session from remote device.
        CADisconnectTCPSession(index - 1);
    }

    u_arraylist_destroy(caglobals.tcp.svrlist);
    caglobals.tcp.svrlist = NULL;

    oc_mutex_unlock(g_mutexObjectList);

#ifdef __WITH_TLS__
    CAcloseSslConnectionAll();
#endif

}

CATCPSessionInfo_t *CAGetTCPSessionInfoFromEndpoint(const CAEndpoint_t *endpoint, size_t *index)
{
    VERIFY_NON_NULL_RET(endpoint, TAG, "endpoint is NULL", NULL);
    VERIFY_NON_NULL_RET(index, TAG, "index is NULL", NULL);

    OIC_LOG_V(DEBUG, TAG, "Looking for [%s:%d]", endpoint->addr, endpoint->port);

    // get connection info from list
    uint32_t length = u_arraylist_length(caglobals.tcp.svrlist);
    for (size_t i = 0; i < length; i++)
    {
        CATCPSessionInfo_t *svritem = (CATCPSessionInfo_t *) u_arraylist_get(
                caglobals.tcp.svrlist, i);
        if (!svritem)
        {
            continue;
        }

        if (!strncmp(svritem->sep.endpoint.addr, endpoint->addr,
                     sizeof(svritem->sep.endpoint.addr))
                && (svritem->sep.endpoint.port == endpoint->port)
                && (svritem->sep.endpoint.flags & endpoint->flags))
        {
            OIC_LOG(DEBUG, TAG, "Found in session list");
            *index = i;
            return svritem;
        }
    }

    OIC_LOG(DEBUG, TAG, "Session not found");
    return NULL;
}

CASocketFd_t CAGetSocketFDFromEndpoint(const CAEndpoint_t *endpoint)
{
    VERIFY_NON_NULL_RET(endpoint, TAG, "endpoint is NULL", OC_INVALID_SOCKET);

    OIC_LOG_V(DEBUG, TAG, "Looking for [%s:%d]", endpoint->addr, endpoint->port);

    // get connection info from list.
    oc_mutex_lock(g_mutexObjectList);
    uint32_t length = u_arraylist_length(caglobals.tcp.svrlist);
    for (size_t i = 0; i < length; i++)
    {
        CATCPSessionInfo_t *svritem = (CATCPSessionInfo_t *) u_arraylist_get(
                caglobals.tcp.svrlist, i);
        if (!svritem)
        {
            continue;
        }

        if (!strncmp(svritem->sep.endpoint.addr, endpoint->addr,
                     sizeof(svritem->sep.endpoint.addr))
                && (svritem->sep.endpoint.port == endpoint->port)
                && (svritem->sep.endpoint.flags & endpoint->flags))
        {
            oc_mutex_unlock(g_mutexObjectList);
            OIC_LOG(DEBUG, TAG, "Found in session list");
            return svritem->fd;
        }
    }

    oc_mutex_unlock(g_mutexObjectList);
    OIC_LOG(DEBUG, TAG, "Session not found");
    return OC_INVALID_SOCKET;
}

CATCPSessionInfo_t *CAGetSessionInfoFromFD(CASocketFd_t fd, size_t *index)
{
    oc_mutex_lock(g_mutexObjectList);

    // check from the last item.
    CATCPSessionInfo_t *svritem = NULL;
    uint32_t length = u_arraylist_length(caglobals.tcp.svrlist);
    for (size_t i = 0; i < length; i++)
    {
        svritem = (CATCPSessionInfo_t *) u_arraylist_get(caglobals.tcp.svrlist, i);

        if (svritem && svritem->fd == fd)
        {
            *index = i;
            oc_mutex_unlock(g_mutexObjectList);
            return svritem;
        }
    }

    oc_mutex_unlock(g_mutexObjectList);

    return NULL;
}

CAResult_t CASearchAndDeleteTCPSession(const CAEndpoint_t *endpoint)
{
    oc_mutex_lock(g_mutexObjectList);

    CAResult_t result = CA_STATUS_OK;
    size_t index = 0;
    CATCPSessionInfo_t *svritem = CAGetTCPSessionInfoFromEndpoint(endpoint, &index);
    if (svritem)
    {
        result = CADisconnectTCPSession(index);
        if (CA_STATUS_OK != result)
        {
            OIC_LOG_V(ERROR, TAG, "CADisconnectTCPSession failed, result[%d]", result);
        }
    }

    oc_mutex_unlock(g_mutexObjectList);
    return result;
}

size_t CAGetTotalLengthFromHeader(const unsigned char *recvBuffer)
{
    OIC_LOG(DEBUG, TAG, "IN - CAGetTotalLengthFromHeader");

    coap_transport_t transport = coap_get_tcp_header_type_from_initbyte(
            ((unsigned char *)recvBuffer)[0] >> 4);
    size_t optPaylaodLen = coap_get_length_from_header((unsigned char *)recvBuffer,
                                                        transport);
    size_t headerLen = coap_get_tcp_header_length((unsigned char *)recvBuffer);

    OIC_LOG_V(DEBUG, TAG, "option/paylaod length [%zu]", optPaylaodLen);
    OIC_LOG_V(DEBUG, TAG, "header length [%zu]", headerLen);
    OIC_LOG_V(DEBUG, TAG, "total data length [%zu]", headerLen + optPaylaodLen);

    OIC_LOG(DEBUG, TAG, "OUT - CAGetTotalLengthFromHeader");
    return headerLen + optPaylaodLen;
}

void CATCPSetErrorHandler(CATCPErrorHandleCallback errorHandleCallback)
{
    g_tcpErrorHandler = errorHandleCallback;
}

