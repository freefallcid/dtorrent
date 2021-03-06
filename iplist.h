#ifndef IPLIST_H
#define IPLIST_H

#include "def.h"

#ifdef WINDOWS
#include <Winsock2.h>
#else
#include <unistd.h>
#include <stdio.h>   // autoconf manual: Darwin + others prereq for stdlib.h
#include <stdlib.h>  // autoconf manual: Darwin prereq for sys/socket.h
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include "bttypes.h"

typedef struct _iplist{
  struct sockaddr_in address;
  struct _iplist *next;
}IPLIST;

class IpList
{
private:
  IPLIST *ipl_head;
  dt_count_t count;
  void _Empty();
public:
  IpList(){ ipl_head = (IPLIST *)0; count = 0; }
  ~IpList(){ if(ipl_head) _Empty(); }
  int Add(const struct sockaddr_in *psin);
  int Pop(struct sockaddr_in *psin);
  int IsEmpty() const { return count ? 0 : 1; }
};

extern IpList IPQUEUE;

#endif  // IPLIST_H

