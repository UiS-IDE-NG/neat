/*
 * Socket API implementation for NEAT
 * Copyright (C) 2016-2017 by Thomas Dreibholz <dreibh@simula.no>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of NEAT nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "neat-socketapi-internals.h"

#include <stddef.h>
#include <stdlib.h>

#include <errno.h>
#include <assert.h>


NEAT_EXTERN neat_error_code neat_writev(struct neat_ctx *ctx, struct neat_flow *flow,
                                        const struct iovec *iov, size_t iovlen,
                                        struct neat_tlv optional[], unsigned int opt_count)
{
   // FIXME: Scatter/gather I/O not yet implemented!
   assert(iovlen == 1);
   return(neat_write(ctx, flow, iov[0].iov_base, iov[0].iov_len,
                     optional, opt_count));
}

NEAT_EXTERN neat_error_code neat_readv(struct neat_ctx *ctx, struct neat_flow *flow,
                                       struct iovec *iov, size_t iovlen, uint32_t *actualAmt,
                                       struct neat_tlv optional[], unsigned int opt_count)
{
   // FIXME: Scatter/gather I/O not yet implemented!
   assert(iovlen == 1);
   return(neat_read(ctx, flow, iov[0].iov_base, iov[0].iov_len, actualAmt,
                    optional, opt_count));
}


/* ###### NEAT sendmsg() implementation ################################## */
ssize_t nsa_sendmsg(int sockfd, const struct msghdr* msg, int flags)
{
   GET_NEAT_SOCKET(sockfd)
   if(neatSocket->ns_flow != NULL) {

      /* ====== Write to socket ========================================== */
      pthread_mutex_lock(&neatSocket->ns_mutex);
      neat_error_code result =
         neat_writev(gSocketAPIInternals->nsi_neat_context, neatSocket->ns_flow,
                     msg->msg_iov, msg->msg_iovlen,
                     NULL, 0);
      if( (result == NEAT_ERROR_WOULD_BLOCK) &&
          (!(neatSocket->ns_flags & NSAF_NONBLOCKING)) &&
          (!(flags & MSG_DONTWAIT)) ) {
         /* ====== Blocking mode: wait and try again ===================== */
         pthread_mutex_unlock(&neatSocket->ns_mutex);
         nsa_wait_for_event(neatSocket, POLLOUT|POLLERR, -1);
         pthread_mutex_lock(&neatSocket->ns_mutex);
         result =  neat_writev(gSocketAPIInternals->nsi_neat_context, neatSocket->ns_flow,
                               msg->msg_iov, msg->msg_iovlen,
                               NULL, 0);
      }

      pthread_mutex_unlock(&neatSocket->ns_mutex);

      /* ====== Handle result ============================================ */
      switch(result) {
         case NEAT_OK:
            return(0);
          break;
         case NEAT_ERROR_WOULD_BLOCK:
             errno = EAGAIN;
             return(-1);
          break;
         case NEAT_ERROR_IO:
             errno = EIO;
             return(-1);
          break;
         case NEAT_ERROR_BAD_ARGUMENT:
             errno = EINVAL;
             return(-1);
          break;
         case NEAT_ERROR_OUT_OF_MEMORY:
             errno = ENOMEM;
             return(-1);
          break;
      }

      errno = ENOENT;   /* Unexpected error from NEAT Core */
      return(-1);
   }
   else {
      return(sendmsg(neatSocket->ns_socket_sd, msg, flags));
   }
}


/* ###### NEAT recvmsg() implementation ################################## */
ssize_t nsa_recvmsg(int sockfd, struct msghdr* msg, int flags)
{
   GET_NEAT_SOCKET(sockfd)
   if(neatSocket->ns_flow != NULL) {
      uint32_t actual_amount = 0;

      /* ====== Read from socket ========================================= */
      pthread_mutex_lock(&neatSocket->ns_mutex);
      neat_error_code result =
         neat_readv(gSocketAPIInternals->nsi_neat_context, neatSocket->ns_flow,
                    msg->msg_iov, msg->msg_iovlen, &actual_amount,
                    NULL, 0);
      if( (result == NEAT_ERROR_WOULD_BLOCK) &&
          (!(neatSocket->ns_flags & NSAF_NONBLOCKING)) &&
          (!(flags & MSG_DONTWAIT)) ) {
         /* ====== Blocking mode: wait and try again ===================== */
         pthread_mutex_unlock(&neatSocket->ns_mutex);
         nsa_wait_for_event(neatSocket, POLLIN|POLLERR, -1);
         pthread_mutex_lock(&neatSocket->ns_mutex);
         result = neat_readv(gSocketAPIInternals->nsi_neat_context, neatSocket->ns_flow,
                             msg->msg_iov, msg->msg_iovlen, &actual_amount,
                             NULL, 0);

      }
      pthread_mutex_unlock(&neatSocket->ns_mutex);

      /* ====== Handle result ============================================ */
      switch(result) {
         case NEAT_OK:
            return((ssize_t)actual_amount);
          break;
         case NEAT_ERROR_WOULD_BLOCK:
             errno = EAGAIN;
             return(-1);
          break;
         case NEAT_ERROR_IO:
             errno = EIO;
             return(-1);
          break;
         case NEAT_ERROR_MESSAGE_TOO_BIG:
             errno = EMSGSIZE;
             return(-1);
          break;
         case NEAT_ERROR_BAD_ARGUMENT:
             errno = EINVAL;
             return(-1);
          break;
      }

      errno = ENOENT;   /* Unexpected error from NEAT Core */
      return(-1);
   }
   else {
      return(recvmsg(neatSocket->ns_socket_sd, msg, flags));
   }
}


/* ###### NEAT write() implementation #################################### */
ssize_t nsa_write(int fd, const void* buf, size_t len)
{
   struct iovec  iov = { (char*)buf, len };
   struct msghdr msg = {
      NULL, 0,
      &iov, 1,
      NULL, 0,
      0
   };
   return(nsa_sendmsg(fd, &msg, 0));
}


/* ###### NEAT send() implementation ##################################### */
ssize_t nsa_send(int sockfd, const void* buf, size_t len, int flags)
{
   struct iovec  iov = { (char*)buf, len };
   struct msghdr msg = {
      NULL, 0,
      &iov, 1,
      NULL, 0,
      flags
   };
   return(nsa_sendmsg(sockfd, &msg, flags));
}


/* ###### NEAT sendto() implementation ################################### */
ssize_t nsa_sendto(int sockfd, const void* buf, size_t len, int flags,
                   const struct sockaddr* to, socklen_t tolen)
{
   struct iovec  iov = { (char*)buf, len };
   struct msghdr msg = {
      (void*)to,
      tolen,
      &iov, 1,
      NULL, 0,
      flags
   };
   return(nsa_sendmsg(sockfd, &msg, flags));
}


/* ###### NEAT sendv() implementation #################################### */
ssize_t nsa_sendv(int sockfd, const void* buf, size_t len,
                  struct sockaddr* to, int addrcnt,
                  void *info, socklen_t infolen, unsigned int infotype,
                  int flags)
{
   abort();   // FIXME!
}


/* ###### NEAT read() implementation ##################################### */
ssize_t nsa_read(int fd, void* buf, size_t len)
{
   struct iovec  iov = { (char*)buf, len };
   struct msghdr msg = {
      NULL, 0,
      &iov, 1,
      NULL, 0,
      0
   };
   return(nsa_recvmsg(fd, &msg, 0));
}


/* ###### NEAT recv() implementation ##################################### */
ssize_t nsa_recv(int sockfd, void* buf, size_t len, int flags)
{
   struct iovec  iov = { (char*)buf, len };
   struct msghdr msg = {
      NULL, 0,
      &iov, 1,
      NULL, 0,
      flags
   };
   return(nsa_recvmsg(sockfd, &msg, flags));
}


/* ###### NEAT recvfrom() implementation ################################# */
ssize_t nsa_recvfrom(int sockfd,  void* buf, size_t len, int flags,
                     struct sockaddr* from, socklen_t* fromlen)
{
   struct iovec  iov = { (char*)buf, len };
   struct msghdr msg = {
      (void*)from, *fromlen,
      &iov, 1,
      NULL, 0,
      flags
   };
   const int result = nsa_recvmsg(sockfd, &msg, flags);
   if(fromlen) {
      *fromlen = msg.msg_namelen;
   }
   return(result);
}


/* ###### NEAT recvv() implementation #################################### */
ssize_t nsa_recvv(int sockfd, void* buf, size_t len,
                  struct sockaddr* from, socklen_t* fromlen,
                  void* info, socklen_t* infolen, unsigned int* infotype,
                  int* msg_flags)
{
   abort();   // FIXME!
}
