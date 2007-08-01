#ifndef TRACKER_H
#define TRACKER_H

#include <sys/types.h>

#include "./def.h"
#include "./bufio.h"

#ifdef WINDOWS
#include <Winsock2.h>

#else
#include <unistd.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/param.h>
#endif

#define T_FREE 		0
#define T_CONNECTING	1
#define T_READY		2
#define T_FINISHED	3

class btTracker
{
 private:
  char m_host[MAXHOSTNAMELEN];
  char m_path[MAXPATHLEN];
  int m_port;

  struct sockaddr_in m_sin;

  unsigned char m_status:2;
  unsigned char m_f_started:1;
  unsigned char m_f_stoped:1;
  unsigned char m_f_completed:1;

  unsigned char m_f_pause:1;
  unsigned char m_f_reserved:2;


  time_t m_interval;		// ��Trackerͨ�ŵ�ʱ����
  time_t m_last_timestamp;	// ���һ�γɹ���Trackerͨ�ŵ�ʱ��
  size_t m_connect_refuse_click;

  size_t m_ok_click;	// tracker ok response counter
  size_t m_peers_count;	// total number of peers
  size_t m_prevpeers;	// number of peers previously seen

  SOCKET m_sock;
  BufIo m_reponse_buffer;
  
  int _IPsin(char *h, int p, struct sockaddr_in *psin);
  int _s2sin(char *h,int p,struct sockaddr_in *psin);
  int _UpdatePeerList(char *buf,size_t bufsiz);

 public:
  btTracker();
  ~btTracker();

  int Initial();

  void Reset(time_t new_interval);

  unsigned char GetStatus() { return m_status;}
  void SetStatus(unsigned char s) { m_status = s; }

  SOCKET GetSocket() { return m_sock; }

  void SetPause() { m_f_pause = 1; }
  void ClearPause() { m_f_pause = 0; }

  void SetStoped() { Reset(15); m_f_stoped = 1; m_last_timestamp -= 15;}

  int Connect();
  int SendRequest();
  int CheckReponse();
  int IntervalCheck(const time_t *pnow,fd_set* rfdp, fd_set *wfdp);
  int SocketReady(fd_set *rfdp, fd_set *wfdp, int *nfds);

  size_t GetRefuseClick() const { return m_connect_refuse_click; }
  size_t GetOkClick() const { return m_ok_click; }
  size_t GetPeersCount() const { return m_peers_count; }
};

extern btTracker Tracker;

#endif
