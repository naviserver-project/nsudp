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
 *   Configure in the NaviServer config file:
 *
 *   ###############################################
 *   ...
 *   ns_section    ns/servers/server/modules
 *   ns_param      nsudp        nsudp.so
 *
 *   ns_section    ns/servers/server/module/nsudp
 *   ns_param      address    0.0.0.0
 *   ns_param      port       80
 *   ...
 *   ###############################################
 *
 * 
 * To send udp packages, use:
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

#define UDP_VERSION  "0.1"

typedef struct {
   int packetsize;
} UdpDriver;

/*
 * Local functions defined in this file.
 */

static Ns_DriverListenProc Listen;
static Ns_DriverAcceptProc Accept;
static Ns_DriverRecvProc Recv;
static Ns_DriverSendProc Send;
static Ns_DriverKeepProc Keep;
static Ns_DriverCloseProc Close;

static int UdpInterpInit(Tcl_Interp *interp, void *arg);
static int UdpCmd(ClientData arg, Tcl_Interp *interp,int objc,Tcl_Obj *CONST objv[]);

NS_EXPORT int Ns_ModuleVersion = 1;

NS_EXPORT int Ns_ModuleInit(char *server, char *module)
{
    char *path;
    UdpDriver *drvPtr;
    Ns_DriverInitData init = {0};

    path = Ns_ConfigGetPath(server,module,NULL);
    drvPtr = ns_calloc(1, sizeof(UdpDriver));
    drvPtr->packetsize = Ns_ConfigIntRange(path, "packetsize", -1, -1, INT_MAX);

    init.version = NS_DRIVER_VERSION_2;
    init.name = "nsudp";
    init.listenProc = Listen;
    init.acceptProc = Accept;
    init.recvProc = Recv;
    init.requestProc = NULL;
    init.sendProc = Send;
    init.sendFileProc = NULL;
    init.keepProc = Keep;
    init.closeProc = Close;
    init.opts = NS_DRIVER_ASYNC;
    init.arg = drvPtr;
    init.path = path;

    Ns_TclRegisterTrace(server, UdpInterpInit, drvPtr, NS_TCL_TRACE_CREATE);

    return Ns_DriverInit(server, module, &init);
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
 * Listen --
 *
 *      Open a listening UDP socket in non-blocking mode.
 *
 * Results:
 *      The open socket or INVALID_SOCKET on error.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static SOCKET
Listen(Ns_Driver *driver, CONST char *address, int port, int backlog)
{
    SOCKET sock;

    sock = Ns_SockListenUdp((char*)address, port);
    if (sock != INVALID_SOCKET) {
        (void) Ns_SockSetNonBlocking(sock);
    }
    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * Accept --
 *
 *      Accept a new TCP socket in non-blocking mode.
 *
 * Results:
 *      NS_DRIVER_ACCEPT_DATA  - socket accepted, data present
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
 
static NS_DRIVER_ACCEPT_STATUS
Accept(Ns_Sock *sock, SOCKET listensock,
       struct sockaddr *sockaddrPtr, int *socklenPtr)
{
    sock->sock = listensock;
    return NS_DRIVER_ACCEPT_DATA;
}


/*
 *----------------------------------------------------------------------
 *
 * Recv --
 *
 *      Receive data into given buffers.
 *
 * Results:
 *      Total number of bytes received or -1 on error or timeout.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static ssize_t
Recv(Ns_Sock *sock, struct iovec *bufs, int nbufs,
     Ns_Time *timeoutPtr, int flags)
{
     socklen_t size = sizeof(struct sockaddr_in);

     return recvfrom(sock->sock, bufs->iov_base, bufs->iov_len, 0, (struct sockaddr*)&sock->sa, &size);
}


/*
 *----------------------------------------------------------------------
 *
 * Send --
 *
 *      Send data from given buffers.
 *
 * Results:
 *      Total number of bytes sent or -1 on error or timeout.
 *
 * Side effects:
 *      May block once for driver sendwait timeout seconds if first
 *      attempt would block.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
Send(Ns_Sock *sock, struct iovec *bufs, int nbufs,
     Ns_Time *timeoutPtr, int flags)
{
    ssize_t len, size;
    Tcl_DString *ds = sock->arg;
    UdpDriver *drvPtr = sock->driver->arg;

    if (ds == NULL) {
        ds = ns_calloc(1, sizeof(Tcl_DString));
        Tcl_DStringInit(ds);
        sock->arg = ds;
    }

    for (len = size = 0; len < nbufs; len++) {
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
            Ns_Log(Error,"nsudp: %s: FD %d: sendto %" PRIdz " bytes to %s: %s", 
		   sock->driver->name, sock->sock, len, ns_inet_ntoa(sock->sa.sin_addr), strerror(errno));
        }

        /*
         * Move remaining bytes to the beginning of the buffer for the next iteration
         */

        memmove(ds->string, ds->string + len, ds->length - len);
        Tcl_DStringSetLength(ds, ds->length - len);
    }
    return size;
}


/*
 *----------------------------------------------------------------------
 *
 * Keep --
 *
 *      Cannot do keep alives with UDP
 *
 * Results:
 *      0, always.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
Keep(Ns_Sock *sock)
{
    return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * Close --
 *
 *      Close the connection socket.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Does not close UDP socket
 *
 *----------------------------------------------------------------------
 */

static void
Close(Ns_Sock *sock)
{
    Tcl_DString *ds = sock->arg;

    if (ds != NULL) {
        if (ds->length > 0) {
            int len = sendto(sock->sock, ds->string, ds->length, 0, (struct sockaddr*)&sock->sa, sizeof(struct sockaddr_in));
            if (len == -1) {
                Ns_Log(Error,"nsudp: %s: FD %d: sendto %d bytes to %s: %s", 
		       sock->driver->name, sock->sock, ds->length, 
		       ns_inet_ntoa(sock->sa.sin_addr), strerror(errno));
            }
        }
        Tcl_DStringFree(ds);
        sock->arg = NULL;
    }
    sock->sock = -1;
}

static int
UdpCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    fd_set fds;
    unsigned char buf[16384];
    struct timeval tv;
    Tcl_DString ds;
    Tcl_Obj *objd;
    unsigned char *data;
    struct sockaddr_in sa, ba;
    socklen_t salen = sizeof(sa);
    char *address = NULL, *bindaddr = NULL;
    int i, sock, len, port, rc = TCL_OK;
    int stream = 0, timeout = 5, retries = 1, noreply = 0;

    Ns_ObjvSpec opts[] = {
        {"-timeout",  Ns_ObjvInt,    &timeout,  NULL},
        {"-noreply",  Ns_ObjvBool,   &noreply,  (void*)1},
        {"-retries",  Ns_ObjvInt,    &retries,  NULL},
        {"-stream",   Ns_ObjvInt,    &stream,   NULL},
        {"-bind",     Ns_ObjvString, &bindaddr, NULL},
        {"--",        Ns_ObjvBreak,  NULL,      NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"address",  Ns_ObjvString, &address, NULL},
        {"port",     Ns_ObjvInt,    &port,    NULL},
        {"data",     Ns_ObjvObj,    &objd,    NULL},
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

    /* Bind to local address */
    if (bindaddr != NULL && Ns_GetSockAddr(&ba, bindaddr, 0) == NS_OK) {
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(int));
        if (bind(sock, (struct sockaddr *)&ba, sizeof(ba)) != 0) {
            Tcl_AppendResult(interp, "bind error ", strerror(errno), 0);
            ns_sockclose(sock);
            return TCL_ERROR;
        }
    }

    data = Tcl_GetByteArrayFromObj(objd, &len);

resend:

    Ns_Log(Notice, "nsudp: sending %d bytes to %s:%d from %s", len, ns_inet_ntoa(sa.sin_addr), ntohs(sa.sin_port), ns_inet_ntoa(ba.sin_addr));

    if (sendto(sock, data, len, 0, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        Tcl_AppendResult(interp, "sendto error ", strerror(errno), 0);
        ns_sockclose(sock);
        return TCL_ERROR;
    }
    if (noreply) {
        ns_sockclose(sock);
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
    ns_sockclose(sock);
    Tcl_SetObjResult(interp, Tcl_NewByteArrayObj((unsigned char*)ds.string, ds.length));
    Tcl_DStringFree(&ds);
    return rc;
}
