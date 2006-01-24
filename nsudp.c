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

#include "nsd.h"
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

static Ns_DriverProc udpProc;
static int UdpInterpInit(Tcl_Interp *interp, void *arg);
static int UdpCmd(ClientData arg, Tcl_Interp *interp,int objc,Tcl_Obj *CONST objv[]);

NS_EXPORT int Ns_ModuleVersion = 1;

NS_EXPORT int Ns_ModuleInit(char *server, char *module)
{
    Ns_DriverInitData init;

    init.version = NS_DRIVER_VERSION_1;
    init.name = "nsudp";
    init.proc = udpProc;
    init.opts = NS_DRIVER_UDP;
    init.arg = 0;
    init.path = NULL;

    if (Ns_DriverInit(server, module, &init) != NS_OK) {
        Ns_Log(Error, "nsudp: driver init failed.");
        return NS_ERROR;
    }
    Ns_TclRegisterTrace(server, UdpInterpInit, 0, NS_TCL_TRACE_CREATE);
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
    int len;
    Tcl_DString *ds;
    Sock *sockPtr = (Sock*)sock;
    int slen = sizeof(struct sockaddr_in);

    switch(cmd) {
     case DriverRecv:
         len = recvfrom(sock->sock, bufs->iov_base, bufs->iov_len, 0, (struct sockaddr*)&sockPtr->sa, &slen);
         return len;

     case DriverSend:
         ds = sock->arg;
         if (ds == NULL) {
             ds = ns_calloc(1,sizeof(Tcl_DString));
             Tcl_DStringInit(ds);
             sock->arg = ds; 
         }
         for (slen = 0,len = 0;len < nbufs;len++) {
             Tcl_DStringAppend(ds, bufs[len].iov_base, bufs[len].iov_len);
             slen += bufs[len].iov_len;
         }
         return slen > 0 ? slen : -1;

     case DriverClose:
         ds = sock->arg;
         if (ds != NULL) {
             slen = sendto(sock->sock, ds->string, ds->length, 0, (struct sockaddr*)&sockPtr->sa, slen);
             // Report about failed replies
             if (slen == -1) {
                 Ns_Log(Error,"DriverClose: %s: socket error: %s",sock->driver->name,strerror(errno));
             }
             Tcl_DStringFree(ds);
             ns_free(sock->arg);
             sock->arg = 0;
         }
	 sock->sock = -1;
         return NS_OK;

     case DriverKeep:
         break;
    }
    return NS_ERROR;
}

static int
UdpCmd(ClientData arg, Tcl_Interp *interp,int objc,Tcl_Obj *CONST objv[])
{
    fd_set fds;
    char buf[16384];
    struct timeval tv;
    struct sockaddr_in sa;
    int salen = sizeof(sa);
    char *address = 0, *data = 0;
    int i, sock, len, port, timeout = 5, retries = 1, noreply = 0;
        
    Ns_ObjvSpec opts[] = {
        {"-timeout", Ns_ObjvInt,   &timeout, NULL},
        {"-noreply", Ns_ObjvInt,   &noreply, NULL},
        {"-retries", Ns_ObjvInt,   &retries, NULL},
        {"--",      Ns_ObjvBreak,  NULL,    NULL},
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
        sprintf(buf, "%s:%d", address, port);
        Tcl_AppendResult(interp, "invalid address ", address, 0);
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
    if (sendto(sock, data, len, 0,(struct sockaddr*)&sa,sizeof(sa)) < 0) {
        Tcl_AppendResult(interp, "sendto error ", strerror(errno), 0);
        return TCL_ERROR;
    }
    if (noreply) {
        close(sock);
        return TCL_OK;
    }
    memset(buf,0,sizeof(buf));
    Ns_SockSetNonBlocking(sock);
wait:
    FD_ZERO(&fds);
    FD_SET(sock,&fds);
    tv.tv_sec = timeout;
    tv.tv_usec = 0;
    len = select(sock+1, &fds, 0, 0, &tv);
    switch (len) {
     case -1:
         if (errno == EINTR || errno == EINPROGRESS || errno == EAGAIN) {
             goto wait;
         }
         Tcl_AppendResult(interp, "select error ", strerror(errno), 0);
         close(sock);
         return TCL_ERROR;

     case 0:
         if(--retries < 0) {
            goto resend;
         }
         Tcl_AppendResult(interp, "timeout", 0);
         close(sock);
         return TCL_ERROR;
    }
    if (FD_ISSET(sock, &fds)) {
        len = recvfrom(sock, buf, sizeof(buf)-1, 0, (struct sockaddr*)&sa, &salen);
        if (len > 0) {
            Tcl_AppendResult(interp, buf, 0);
        }
    }
    close(sock);
    return TCL_OK;
}
