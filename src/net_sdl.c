//
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//     Networking module which uses SDL_net
//

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "doomtype.h"
#include "i_system.h"
#include "m_argv.h"
#include "m_misc.h"
#include "net_defs.h"
#include "net_io.h"
#include "net_packet.h"
#include "net_sdl.h"
#include "z_zone.h"

#include <stdint.h>
#include <assert.h>

//
// NETWORKING
//

#if XBOX
#include "../../net.h"
#else
#include <SDL_net.h>
#endif

#define DEFAULT_PORT 2342
#define MAX_SOCKETS 32
#define MAX_PACKET_SIZE 1500
#define HAPROXY_MAX_BUF 108

#define ENFORCE_PROXY 0
#define SIMULATE_PROXY_CONNECTION 0
#define ALLOW_REENTRY 0

static boolean initted = false;
static int port = DEFAULT_PORT;

#define IS_TCP 1

#ifdef IS_TCP
TCPsocket tcpsocket;

TCPsocket serverconnections[MAX_SOCKETS];
uint32_t actual_ip_list[MAX_SOCKETS];

SDLNet_SocketSet clientsocketSet;
SDLNet_SocketSet serversocketSet;
#else
static UDPsocket udpsocket;
static UDPpacket *recvpacket;
#endif

typedef struct
{
    net_addr_t net_addr;
    IPaddress sdl_addr;
} addrpair_t;

static addrpair_t **addr_table;
static int addr_table_size = -1;

// Initializes the address table

static void NET_SDL_InitAddrTable(void)
{
    addr_table_size = 16;

    addr_table = Z_Malloc(sizeof(addrpair_t *) * addr_table_size,
                          PU_STATIC, 0);
    memset(addr_table, 0, sizeof(addrpair_t *) * addr_table_size);
}

static boolean AddressesEqual(IPaddress *a, IPaddress *b)
{
    return a->host == b->host
        && a->port == b->port;
}


uint32_t ipStringToID(char* ip_string)
{
    char packet[strlen(ip_string)];
    uint32_t ipaddr = 0;
    uint32_t iparray[4];

    memset(packet, 0, strlen(ip_string));
    memcpy(packet, ip_string, strlen(ip_string));

    sscanf(packet, "%d.%d.%d.%d", &iparray[0],&iparray[1],&iparray[2],&iparray[3]);

    /*for(int i = 0; i < 4; i++) {
        ipaddr = ipaddr << 8;
        ipaddr = ipaddr + iparray[i];
    }*/

    return iparray[2];
}

static boolean checkIsProxyPacket(char *packet)
{
    return strstr(packet, "PROXY") != NULL;
}

static uint32_t getIPfromProxyPacket(char *packet)
{
    uint32_t src_ip = 0;
    //IPaddress src_ip;

    char ip_string[16];

    memset(ip_string, 0, 16);

    //get ip out of buffer
    sscanf(packet, "%*s %*s %s", ip_string);

    //convert to uint_3
    src_ip = ipStringToID(ip_string);

    return src_ip;
}

// Finds an address by searching the table.  If the address is not found,
// it is added to the table.

static net_addr_t *NET_SDL_FindAddress(IPaddress *addr)
{
    addrpair_t *new_entry;
    int empty_entry = -1;
    int i;

    if (addr_table_size < 0)
    {
        NET_SDL_InitAddrTable();
    }

    for (i=0; i<addr_table_size; ++i)
    {
        if (addr_table[i] != NULL
         && AddressesEqual(addr, &addr_table[i]->sdl_addr))
        {
            return &addr_table[i]->net_addr;
        }

        if (empty_entry < 0 && addr_table[i] == NULL)
            empty_entry = i;
    }

    // Was not found in list.  We need to add it.

    // Is there any space in the table? If not, increase the table size

    if (empty_entry < 0)
    {
        addrpair_t **new_addr_table;
        int new_addr_table_size;

        // after reallocing, we will add this in as the first entry
        // in the new block of memory

        empty_entry = addr_table_size;
        
        // allocate a new array twice the size, init to 0 and copy 
        // the existing table in.  replace the old table.

        new_addr_table_size = addr_table_size * 2;
        new_addr_table = Z_Malloc(sizeof(addrpair_t *) * new_addr_table_size,
                                  PU_STATIC, 0);
        memset(new_addr_table, 0, sizeof(addrpair_t *) * new_addr_table_size);
        memcpy(new_addr_table, addr_table, 
               sizeof(addrpair_t *) * addr_table_size);
        Z_Free(addr_table);
        addr_table = new_addr_table;
        addr_table_size = new_addr_table_size;
    }

    // Add a new entry

    new_entry = Z_Malloc(sizeof(addrpair_t), PU_STATIC, 0);

    new_entry->sdl_addr = *addr;
    new_entry->net_addr.refcount = 0;
    new_entry->net_addr.handle = &new_entry->sdl_addr;
    new_entry->net_addr.module = &net_sdl_module;

    addr_table[empty_entry] = new_entry;

    return &new_entry->net_addr;
}

static void NET_SDL_FreeAddress(net_addr_t *addr)
{
    int i;

    for (i=0; i<addr_table_size; ++i)
    {
        if (addr == &addr_table[i]->net_addr)
        {
            Z_Free(addr_table[i]);
            addr_table[i] = NULL;
            return;
        }
    }

    I_Error("NET_SDL_FreeAddress: Attempted to remove an unused address!");
}


net_addr_t *NET_SDL_ResolveAddress(const char *address)
{
    IPaddress ip;
    char *addr_hostname;
    int addr_port;
    int result;
    char *colon;

    colon = strchr(address, ':');

    addr_hostname = M_StringDuplicate(address);
    if (colon != NULL)
    {
        addr_hostname[colon - address] = '\0';
        addr_port = atoi(colon + 1);
    }
    else
    {
        addr_port = port;
    }

    result = SDLNet_ResolveHost(&ip, addr_hostname, addr_port);

    free(addr_hostname);

    if (result)
    {
        // unable to resolve

        return NULL;
    }
    else
    {
        return NET_SDL_FindAddress(&ip);
    }
}


void NET_SDL_AddrToString(net_addr_t *addr, char *buffer, int buffer_len)
{
    IPaddress *ip;
    uint32_t host;
    uint16_t port;

    ip = (IPaddress *) addr->handle;
    host = SDLNet_Read32(&ip->host);
    port = SDLNet_Read16(&ip->port);

    M_snprintf(buffer, buffer_len, "%i.%i.%i.%i",
               (host >> 24) & 0xff, (host >> 16) & 0xff,
               (host >> 8) & 0xff, host & 0xff);

    // If we are using the default port we just need to show the IP address,
    // but otherwise we need to include the port. This is important because
    // we use the string representation in the setup tool to provided an
    // address to connect to.
    if (port != DEFAULT_PORT)
    {
        char portbuf[10];
        M_snprintf(portbuf, sizeof(portbuf), ":%i", port);
        M_StringConcat(buffer, portbuf, buffer_len);
    }
}

#if XBOX
extern char xbox_ip_str[16];
extern char central_server_ip_str[16];
#endif

static boolean NET_SDL_InitClient(void)
{
#ifdef IS_TCP
    int p;
    int port = 0;
    IPaddress ip;
    int status;
    int bytes_sent;

    const char* host = "127.0.0.1";

    if (initted)
        return true;

    //!
    // @category net
    // @arg <n>
    //
    // Use the specified UDP port for communications, instead of
    // the default (2342).
    //

    p = M_CheckParmWithArgs("-port", 1);
    if (p > 0)
        port = atoi(myargv[p+1]);

    if (port == 0) {
        port = DEFAULT_PORT;
    }

    p = M_CheckParmWithArgs("-connect", 1);
    if (p > 0)
        host = myargv[p+1];

    printf("host %s, port %d\n", host, port);

    SDLNet_Init();

    if(SDLNet_ResolveHost(&ip,host,port)==-1) {
        printf("NET_SDL_InitClient: SDLNet_ResolveHost ERROR");
    }

    //printf("opening\n");
    tcpsocket = SDLNet_TCP_Open(&ip);

    //printf("attempt made\n");
    if (tcpsocket == NULL)
    {
        I_Error("NET_SDL_InitClient: Unable to open a socket host: %x!", ip.host);
    }

#if SIMULATE_PROXY_CONNECTION
    char proxy_packet_buffer[HAPROXY_MAX_BUF];
    // Send dummy proxy string with current IP addr
    snprintf(proxy_packet_buffer, sizeof(proxy_packet_buffer), "PROXY TCP4 %s %s %d %d\r\n",

#ifdef XBOX

        xbox_ip_str,
        central_server_ip_str,
#else
        "127.0.0.1",
        "127.0.0.1",
#endif
        2342,
        2342
        );

    bytes_sent = SDLNet_TCP_Send(tcpsocket, proxy_packet_buffer, strlen(proxy_packet_buffer));
    if (bytes_sent < strlen(proxy_packet_buffer)) {
        I_Error("NET_SDL_SendPacket: Error transmitting packet: %s",
                SDLNet_GetError());
    }

#endif

    //recvpacket = SDLNet_AllocPacket(1500);

#ifdef DROP_PACKETS
    srand(time(NULL));
#endif
    serversocketSet = NULL;
    clientsocketSet = SDLNet_AllocSocketSet(128);
    assert(clientsocketSet != NULL);
    status = SDLNet_TCP_AddSocket(clientsocketSet, tcpsocket);
    assert(status > 0);

    initted = true;

    return true;
#else
    int p;

    if (initted)
        return true;

    //!
    // @category net
    // @arg <n>
    //
    // Use the specified UDP port for communications, instead of
    // the default (2342).
    //

    p = M_CheckParmWithArgs("-port", 1);
    if (p > 0)
        port = atoi(myargv[p+1]);

    SDLNet_Init();

    udpsocket = SDLNet_UDP_Open(0);

    if (udpsocket == NULL)
    {
        I_Error("NET_SDL_InitClient: Unable to open a socket!");
    }

    recvpacket = SDLNet_AllocPacket(1500);

#ifdef DROP_PACKETS
    srand(time(NULL));
#endif

    initted = true;

    return true;
#endif
}

static boolean NET_SDL_InitServer(void)
{
#ifdef IS_TCP
    int p;
    IPaddress ip;

    if (initted)
        return true;

    p = M_CheckParmWithArgs("-port", 1);
    if (p > 0)
        port = atoi(myargv[p+1]);

    if (port == 0) {
        port = DEFAULT_PORT;
    }

    SDLNet_Init();

    //udpsocket = SDLNet_UDP_Open(port);
    printf("binding to %d\n", port);

    if(SDLNet_ResolveHost(&ip, NULL, port)==-1) {
        printf("NET_SDL_InitServer: SDLNet_ResolveHost ERROR");
    }
    tcpsocket = SDLNet_TCP_Open(&ip);

    if (tcpsocket == NULL)
    {
        I_Error("NET_SDL_InitServer: Unable to bind to port %i", port);
    }


#ifdef DROP_PACKETS
    srand(time(NULL));
#endif

    serversocketSet = SDLNet_AllocSocketSet(MAX_SOCKETS);
    clientsocketSet = NULL;

    for(int i = 0; i < MAX_SOCKETS; i++) {
        serverconnections[i] = NULL;
        actual_ip_list[i] = 0xFFFF;
    }

    initted = true;

    return true;
#else
    int p;

    if (initted)
        return true;

    p = M_CheckParmWithArgs("-port", 1);
    if (p > 0)
        port = atoi(myargv[p+1]);

    SDLNet_Init();

    udpsocket = SDLNet_UDP_Open(port);

    if (udpsocket == NULL)
    {
        I_Error("NET_SDL_InitServer: Unable to bind to port %i", port);
    }

    recvpacket = SDLNet_AllocPacket(1500);
#ifdef DROP_PACKETS
    srand(time(NULL));
#endif

    initted = true;

    return true;
#endif
}

static void NET_SDL_SendPacket(net_addr_t *addr, net_packet_t *packet)
{
#ifdef IS_TCP
    uint32_t length;
    IPaddress *remote;
    int found = 0;

    if (serversocketSet == NULL) { // is client
        int bytes_sent;

        assert(tcpsocket != NULL);

        length = packet->len;
        bytes_sent = SDLNet_TCP_Send(tcpsocket, &length, sizeof(length));
        if (bytes_sent < sizeof(length)) {
            I_Error("NET_SDL_SendPacket: Error transmitting packet: %s",
                    SDLNet_GetError());
        }

        bytes_sent = SDLNet_TCP_Send(tcpsocket, packet->data, packet->len);
        if (bytes_sent < packet->len) {
            I_Error("NET_SDL_SendPacket: Error transmitting packet: %s",
                    SDLNet_GetError());
        }
    } else {
        //
        // Server
        //
        for (int i = 0; i < MAX_SOCKETS; i++) {
            TCPsocket conn = serverconnections[i];
            if(conn != NULL) {
                // printf("sending data to %d\n", i);
                remote = SDLNet_TCP_GetPeerAddress(conn);
                if(AddressesEqual((IPaddress *) addr->handle, remote)) {
                    int bytes_sent;

                    uint32_t length = packet->len;
                    bytes_sent = SDLNet_TCP_Send(conn, &length, sizeof(length));
                    if (bytes_sent < sizeof(length)) {
                        // I_Error("NET_SDL_SendPacket: Error transmitting packet: %s",
                        //         SDLNet_GetError());
                        printf("failed to send to client!\n");
                        SDLNet_TCP_DelSocket(serversocketSet, conn);
                        SDLNet_TCP_Close(conn);
                        serverconnections[i] = NULL;
#if ALLOW_REENTRY
                        actual_ip_list[i] = 0xFFFF;
#endif
                        break;
                    }

                    bytes_sent = SDLNet_TCP_Send(conn, packet->data, packet->len);
                    if (bytes_sent < packet->len) {
                        // I_Error("NET_SDL_SendPacket: Error transmitting packet: %s",
                        //         SDLNet_GetError());
                        printf("failed to send to client!\n");
                        SDLNet_TCP_DelSocket(serversocketSet, conn);
                        SDLNet_TCP_Close(conn);
                        serverconnections[i] = NULL;
#if ALLOW_REENTRY
                        actual_ip_list[i] = 0xFFFF;
#endif
                        break;
                    }

                    found = 1;
                    break;
                }
            }
        }
        // assert(found);
    }
#else
    UDPpacket sdl_packet;
    IPaddress ip;

    if (addr == &net_broadcast_addr)
    {
        SDLNet_ResolveHost(&ip, NULL, port);
        ip.host = INADDR_BROADCAST;
    }
    else
    {
        ip = *((IPaddress *) addr->handle);
    }

#if 0
    {
        static int this_second_sent = 0;
        static int lasttime;

        this_second_sent += packet->len + 64;

        if (I_GetTime() - lasttime > TICRATE)
        {
            printf("%i bytes sent in the last second\n", this_second_sent);
            lasttime = I_GetTime();
            this_second_sent = 0;
        }
    }
#endif

#ifdef DROP_PACKETS
    if ((rand() % 4) == 0)
        return;
#endif

    sdl_packet.channel = 0;
    sdl_packet.data = packet->data;
    sdl_packet.len = packet->len;
    sdl_packet.address = ip;

    if (!SDLNet_UDP_Send(udpsocket, -1, &sdl_packet))
    {
        I_Error("NET_SDL_SendPacket: Error transmitting packet: %s",
                SDLNet_GetError());
    }
#endif

}

static boolean NET_SDL_RecvPacket(net_addr_t **addr, net_packet_t **packet)
{
#ifdef IS_TCP
    int length_recv = -1;
    int num_active;
    IPaddress *remote;
    uint32_t actual_ip;

    int added = 0;

    uint32_t length_expected;
    int numServerActiveConnections;
    char *data;

    uint8_t proxy_packet_buffer[HAPROXY_MAX_BUF];
    uint8_t recv_c = '\0';
    size_t packet_index = 0;
    TCPsocket newConnection;
    int should_close = 0;


    //printf("receiving packet\n");

    if (serversocketSet == NULL) {

        //
        // client
        //

        num_active = SDLNet_CheckSockets(clientsocketSet, 0);
        if (num_active < 1) {
            return false;
        }

        //printf("client recv packet\n");
        length_expected = 0;
        length_recv = SDLNet_TCP_Recv(tcpsocket, &length_expected, sizeof(length_expected));
        if (length_recv < sizeof(length_expected)) {
            I_Error("NET_SDL_SendPacket: Error receiving packet 1: %s",
                    SDLNet_GetError());

            // FIXME: add reconnect if socket is broken
        }

        if (length_expected > MAX_PACKET_SIZE) {
            I_Error("NET_SDL_RecvPacket: Error receiving packet 2: %s",
                    SDLNet_GetError());
        }

        // *packet = NET_NewPacket(length_recv); //result == size of msg received
        // assert(*packet != NULL);

        data = malloc(length_expected + 4);
        memset(data, 0, length_expected + 4);
        data[length_expected] = '\xCC';

        // printf("length expected = %d bytes\n", length_expected);

        length_recv = SDLNet_TCP_Recv(tcpsocket, data, length_expected);
        if (length_recv != length_expected) {
            I_Error("NET_SDL_RecvPacket: Error receiving packet 3: %s",
                    SDLNet_GetError());
            free(data);
        }

        assert(data[length_expected] == '\xCC');

        *packet = NET_NewPacket(length_expected);
        assert(*packet != NULL);
        memcpy((*packet)->data, data, length_expected);
        (*packet)->len = length_recv;
        free(data);

        // printf("received %d bytes\n", length_recv);

        // checkme
        *addr = NET_SDL_FindAddress(SDLNet_TCP_GetPeerAddress(tcpsocket));
        assert(*addr != NULL);

        return true;

    } else {

        //
        // server
        //

        while (1) {
            //
            // Check for new connections
            //
            assert(tcpsocket != NULL);

            newConnection = SDLNet_TCP_Accept(tcpsocket);
            if (newConnection == NULL) {
                // No pending connections
                break;
            }

            ///////////////////////////////////// HAPROXY TRACKING
#if ENFORCE_PROXY

            //printf("adding new connection\n");
            added = 0;
            should_close = 0;

            //check if already added

            memset( proxy_packet_buffer, 0, HAPROXY_MAX_BUF );
            packet_index = 0;

            while(1) {
                length_recv = SDLNet_TCP_Recv(newConnection, &recv_c, 1);

                if (length_recv <= 0) {
                    should_close = 1;
                    break;
                }

                if(recv_c == '\n') {
                    proxy_packet_buffer[packet_index] = '\0';
                    break;
                }

                proxy_packet_buffer[packet_index] = recv_c;
                packet_index = packet_index+1;
                if(packet_index >= HAPROXY_MAX_BUF) {
                    printf("max proxy packet size reached without a CRLF\n");
                    should_close = 1;
                    break;
                }
            }

            if (should_close ||
                !checkIsProxyPacket((char*)proxy_packet_buffer)) {
                SDLNet_TCP_Close(newConnection);
                printf("rejecting malformed request (no proxy line)\n");
                continue; // Look for more connections
            }

            // Get the originating IP address from the proxy line
            actual_ip = getIPfromProxyPacket((char*)proxy_packet_buffer);

            // Check for an open sockets with this IP address already
            for (int i = 0; i < MAX_SOCKETS; i++) {
                if (actual_ip_list[i] == actual_ip) {
                    printf("rejecting duplicate connection %x at %d\n", actual_ip, i);
                    SDLNet_TCP_Close(newConnection);
                    return false;
                }
            }
#endif

            for (int i = 0; i < MAX_SOCKETS; i++) {
                if (serverconnections[i] == NULL) {
                    printf("adding newconn to %d\n", i);
                    serverconnections[i] = newConnection;
                    actual_ip_list[i] = actual_ip;
                    SDLNet_TCP_AddSocket(serversocketSet, newConnection);
                    added = 1;
                    break;
                }
            }

            if (!added) {
                // POTENTIAL DOS if one client takes up all available socket slots
                // printf("no free space to add socket\n");
                SDLNet_TCP_Close(newConnection);
            }
        }

        numServerActiveConnections = SDLNet_CheckSockets(serversocketSet, 0);
        if (numServerActiveConnections <= 0) {
            return false;
        }

        // printf("server receiving packet\n");

        // char data[MAX_PACKET_SIZE];
        // memset( data, 0, MAX_PACKET_SIZE );

        for (int i = 0; i < MAX_SOCKETS; i++)
        {
            TCPsocket conn = serverconnections[i];
            if ((conn == NULL) || (SDLNet_SocketReady(conn) == 0)) {
                continue;
            }

            length_recv = SDLNet_TCP_Recv(conn, &length_expected, 4);
            if ((length_recv < sizeof(length_expected))
                || (length_expected > MAX_PACKET_SIZE)) {
                // I_Error("NET_SDL_RecvPacket: Error receiving packet: %s",
                //         SDLNet_GetError());
                // printf("failed to recv, closing socket\n");
                SDLNet_TCP_DelSocket(serversocketSet, conn);
                SDLNet_TCP_Close(conn);
                serverconnections[i] = NULL;
#if ALLOW_REENTRY
                actual_ip_list[i] = 0xFFFF;
#endif 
                continue; // Check other sockets
            }

            data = malloc(length_expected + 4);
            memset(data, 0, length_expected + 4);
            data[length_expected] = '\xCC';

            // printf("length expected = %d bytes\n", length_expected);

            length_recv = SDLNet_TCP_Recv(conn, data, length_expected);
            if (length_recv < length_expected) {
                // I_Error("NET_SDL_RecvPacket: Error receiving packet: %s",
                //         SDLNet_GetError());
                printf("failed to recv, closing socket\n");
                SDLNet_TCP_DelSocket(serversocketSet, conn);
                SDLNet_TCP_Close(conn);
                serverconnections[i] = NULL;
#if ALLOW_REENTRY
                actual_ip_list[i] = 0xFFFF;
#endif
                // NET_FreePacket(*packet);
                free(data);
                continue; // Check other sockets
            }

            assert(data[length_expected] == '\xCC');

            *packet = NET_NewPacket(length_expected);
            assert(*packet != NULL);
            memcpy((*packet)->data, data, length_expected);
            (*packet)->len = length_recv;
            free(data);

            // printf("received %d bytes\n", length_recv);

            remote = SDLNet_TCP_GetPeerAddress(conn);
            *addr = NET_SDL_FindAddress(remote);
            assert(*addr != NULL);

            return true;
        }

        return false;
    }
#else
    int result;
    result = SDLNet_UDP_Recv(udpsocket, recvpacket);

    if (result < 0)
    {
        I_Error("NET_SDL_RecvPacket: Error receiving packet: %s",
                SDLNet_GetError());
    }

    // no packets received

    if (result == 0)
        return false;

    // Put the data into a new packet structure

    *packet = NET_NewPacket(recvpacket->len);
    memcpy((*packet)->data, recvpacket->data, recvpacket->len);
    (*packet)->len = recvpacket->len;

    // Address

    *addr = NET_SDL_FindAddress(&recvpacket->address);

    return true;
#endif
}

// Complete module

net_module_t net_sdl_module =
{
    NET_SDL_InitClient,
    NET_SDL_InitServer,
    NET_SDL_SendPacket,
    NET_SDL_RecvPacket,
    NET_SDL_AddrToString,
    NET_SDL_FreeAddress,
    NET_SDL_ResolveAddress,
};