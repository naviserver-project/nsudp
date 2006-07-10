/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * Copyright (C) 2001-2003 Vlad Seryakov
 * All rights reserved.
 *
 * Alternatively, the contents of this file may be used under the terms
 * of the GNU General Public License (the "GPL"), in which case the
 * provisions of GPL are applicable instead of those above.  If you wish
 * to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the
 * License, indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by the GPL.
 * If you do not delete the provisions above, a recipient may use your
 * version of this file under either the License or the GPL.
 */

/*
 * nsudp.c -- HTTP over UDP driver
 *
 *
 * Usage:
 *
 *   nsd.tcl
 *
 *   ns_section    ns/servers/server/modules
 *   ns_param      nsudp        nsudp.so
 *
 *   ns_section    ns/servers/server/module/nsudp
 *   ns_param      address    0.0.0.0
 *   ns_param      port       80
 *
 *
 *   ns_udp ?-timeout N? ?-noreply? ipaddr port data
 *
 *      ns_udp 127.0.0.1 80 "GET / HTTP/1.0\n\n"
 *
 * Authors
 *
 *     Vlad Seryakov vlad@crystalballinc.com
 */

#define BUFFER_LEN 1024

#include "ns.h"
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <netdb.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <string.h>

#define UDP_VERSION  "0.1"

typedef struct {
   int packetsize;
} UdpDriver;

static Ns_DriverProc udpProc;
static int UdpInterpInit(Tcl_Interp *interp, void *arg);
static int UdpCmd(ClientData arg, Tcl_Interp *interp,int objc,Tcl_Obj *CONST objv[]);

NS_EXPORT int Ns_ModuleVersion = 1;

NS_EXPORT int Ns_ModuleInit(char *server, char *module)
{
    char *path;
    UdpDriver *drvPtr;
    Ns_DriverInitData init;

    drvPtr = ns_calloc(1, sizeof(UdpDriver));

    init.version = NS_DRIVER_VERSION_1;
    init.name = "nsudp";
    init.proc = udpProc;
    init.opts = NS_DRIVER_UDP|NS_DRIVER_ASYNC;
    init.arg = drvPtr;
    init.path = NULL;

    if (Ns_DriverInit(server, module, &init) != NS_OK) {
        Ns_Log(Error, "nsudp: driver init failed.");
        ns_free(drvPtr);
        return NS_ERROR;
    }
    path = Ns_ConfigGetPath(server,module,NULL);
    drvPtr->packetsize = Ns_ConfigIntRange(path, "packetsize", -1, -1, INT_MAX);

    Ns_TclRegisterTrace(server, UdpInterpInit, drvPtr, NS_TCL_TRACE_CREATE);
    return NS_OK;
}

static int
UdpInterpInit(Tcl_Interp *interp, void *arg)
{
    Tcl_CreateObjCommand(interp, "ns_udp", UdpCmd, arg, NULL);
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * udpProc --
 *
 *	Driver proc for UDP requests
 *
 * Results:
 *	NS_OK or NS_ERROR
 *
 * Side effects:
 *  	None
 *
 *----------------------------------------------------------------------
 */

static int
udpProc(Ns_DriverCmd cmd, Ns_Sock *sock, struct iovec *bufs, int nbufs)
{
    int len, size = 0;
    Tcl_DString *ds = sock->arg;
    UdpDriver *drvPtr = sock->driver->arg;

    switch(cmd) {
     case DriverRecv:
         size = sizeof(struct sockaddr_in);
         len = recvfrom(sock->sock, bufs->iov_base, bufs->iov_len, 0, (struct sockaddr*)&sock->sa, (socklen_t*)&size);
         return len;

     case DriverSend:
         if (ds == NULL) {
             ds = ns_calloc(1, sizeof(Tcl_DString));
             Tcl_DStringInit(ds);
             sock->arg = ds;
         }
         for (len = 0; len < nbufs; len++) {
             Tcl_DStringAppend(ds, bufs[len].iov_base, bufs[len].iov_len);
             size += bufs[len].iov_len;
         }

         /*
          * if packetsize is zero that means send every given chunk in separate UDP packet,
          * otherwise try to buffer and send data in packetsize chunks
          */

         while (drvPtr->packetsize > -1 && ds->length >= drvPtr->packetsize) {
             if (drvPtr->packetsize > 0) {
                 len = drvPtr->packetsize;
             } else {
                 len = ds->length;
             }
             len = sendto(sock->sock, ds->string, len, 0, (struct sockaddr*)&sock->sa, sizeof(struct sockaddr_in));
             if (len == -1) {
                 Ns_Log(Error,"DriverSend: %s: FD %d: sendto %d bytes to %s: %s", sock->driver->name, sock->sock, len, ns_inet_ntoa(sock->sa.sin_addr), strerror(errno));
             }

             /*
              * Move remaining bytes to the beginning of the buffer for the next iteration
              */

             memmove(ds->string, ds->string + len, ds->length - len);
             Tcl_DStringSetLength(ds, ds->length - len);
         }
         return size;

     case DriverClose:
         if (ds != NULL) {
             if (ds->length > 0) {
                 len = sendto(sock->sock, ds->string, ds->length, 0, (struct sockaddr*)&sock->sa, sizeof(struct sockaddr_in));
                 if (len == -1) {
                     Ns_Log(Error,"DriverClose: %s: FD %d: sendto %d bytes to %s: %s", sock->driver->name, sock->sock, ds->length, ns_inet_ntoa(sock->sa.sin_addr), strerror(errno));
                 }
             }
             Tcl_DStringFree(ds);
             ns_free(sock->arg);
             sock->arg = 0;
         }
         return NS_OK;

     case DriverKeep:
     case DriverQueue:
         break;
    }
    return NS_ERROR;
}

static int
UdpCmd(ClientData arg, Tcl_Interp *interp,int objc,Tcl_Obj *CONST objv[])
{
    fd_set fds;
    unsigned char buf[16384];
    struct timeval tv;
    Tcl_DString ds;
    struct sockaddr_in sa;
    socklen_t salen = sizeof(sa);
    char *address = 0, *data = 0;
    int i, sock, len, port, rc = TCL_OK;
    int stream = 0, timeout = 5, retries = 1, noreply = 0;

    Ns_ObjvSpec opts[] = {
        {"-timeout",  Ns_ObjvInt,   &timeout, NULL},
        {"-noreply",  Ns_ObjvInt,   &noreply, NULL},
        {"-retries",  Ns_ObjvInt,   &retries, NULL},
        {"-stream",   Ns_ObjvInt,   &stream,  NULL},
        {"--",        Ns_ObjvBreak,  NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"address",  Ns_ObjvString, &address, NULL},
        {"port",  Ns_ObjvInt, &port, NULL},
        {"data",  Ns_ObjvString, &data, &len},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
      return TCL_ERROR;
    }
    if (Ns_GetSockAddr(&sa, address, port) != NS_OK) {
        sprintf((char*)buf, "%s:%d", address, port);
        Tcl_AppendResult(interp, "invalid address ", buf, 0);
        return TCL_ERROR;
    }
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        Tcl_AppendResult(interp, "socket error ", strerror(errno), 0);
        return TCL_ERROR;
    }
    /* To support brodcasting addresses */
    i = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &i, sizeof(int));

resend:
    if (sendto(sock, data, len, 0, (struct sockaddr*)&sa,sizeof(sa)) < 0) {
        Tcl_AppendResult(interp, "sendto error ", strerror(errno), 0);
        return TCL_ERROR;
    }
    if (noreply) {
        close(sock);
        return TCL_OK;
    }
    memset(buf,0,sizeof(buf));
    Ns_SockSetNonBlocking(sock);
    Tcl_DStringInit(&ds);
    do {
       FD_ZERO(&fds);
       FD_SET(sock,&fds);
       tv.tv_sec = timeout;
       tv.tv_usec = 0;
       len = select(sock+1, &fds, 0, 0, &tv);
       switch (len) {
        case -1:
            if (errno == EINTR || errno == EINPROGRESS || errno == EAGAIN) {
                continue;
            }
            Tcl_DStringSetLength(&ds, 0);
            Ns_DStringPrintf(&ds, "select error %s", strerror(errno));
            rc = TCL_ERROR;
            goto done;

        case 0:
            if (stream) {
                goto done;
            }
            if(--retries < 0) {
               goto resend;
            }
            Tcl_DStringSetLength(&ds, 0);
            Ns_DStringPrintf(&ds, "timeout");
            rc = TCL_ERROR;
            goto done;
       }
       if (FD_ISSET(sock, &fds)) {
           len = recvfrom(sock, buf, sizeof(buf)-1, 0, (struct sockaddr*)&sa, &salen);
           if (len > 0) {
               Tcl_DStringAppend(&ds, (char*)buf, len);
           }
       }
    } while (stream);
done:
    close(sock);
    Tcl_SetObjResult(interp, Tcl_NewByteArrayObj((unsigned char*)ds.string, ds.length));
    Tcl_DStringFree(&ds);
    return rc;
}
