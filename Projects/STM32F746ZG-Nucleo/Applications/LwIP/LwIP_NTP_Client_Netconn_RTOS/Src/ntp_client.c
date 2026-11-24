#include "ntp_client.h"
#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/api.h"


#define NTP_THREAD_PRIO  ( tskIDLE_PRIORITY + 4 )


char * ntp_server_list[] = {"1.cn.pool.ntp.org",
                            NULL
                            };
ip_addr_t   ntp_server_addr;
struct tm * synchronized_local_time; 
uint64_t Org_Timestamp; // T1: 客户端发送请求的时间
uint64_t Receive_Timestamp; // T2: 服务器接收请求的时间
uint64_t Transmit_Timestamp; // T3: 服务器发送响应的时间
uint64_t Destination_Timestamp; // T4: 客户端接收响应的时间

static void ntp_process(uint64_t timestamp);
static void ntp_request(void);
//static uint32_t ntp_get_currenttime(void);

extern void RTC_CalendarShow(void);
extern  void RTC_CalendarUpdate(struct tm * newtime);
extern void RTC_GetTime(struct tm * localtime);

/**
 * @brief 获取当前系统时间并转换为NTP时间戳格式
 * @return 64位NTP时间戳（自1900年1月1日起的秒数和分数）
 */
static uint64_t get_system_time_as_ntp_timestamp(void)
{
  struct tm localtm;
  uint32_t sub_seconds;
  uint64_t ntp_timestamp;
  
  // 获取系统时间（秒部分）
  RTC_GetTime(&localtm);
  
  // 计算自1900年起的秒数
  ntp_timestamp = (uint64_t)(mktime(&localtm) + DIFF_SEC_1900_1970) << 32;
  
  // 获取亚秒部分（根据您的硬件实际情况实现）
  // 这里假设使用某种方式获取亚秒值（如SysTick计数器）
  sub_seconds = get_sub_second_value();
  
  // 将亚秒值转换为NTP格式的分数部分
  ntp_timestamp |= (uint64_t)sub_seconds;
  
  return ntp_timestamp;
}

/**
  * @brief  process the time obtained from NTP server.
            Change it to a human-readable format. And consider the timezone
            The NTP timestamp use 1900 as epoch, but the input param of gmtime() function consider 1970 as epoch.
  * @param  None
  * @retval None
  */
static void ntp_process(uint64_t timestamp)
{
  uint64_t local_ntp_timestamp;
  int64_t delay, offset;

  // 计算网络延迟和时钟偏差
  delay = (Destination_Timestamp - Org_Timestamp) - (Transmit_Timestamp - Receive_Timestamp);
  offset = ((Receive_Timestamp - Org_Timestamp) + (Transmit_Timestamp - Destination_Timestamp)) / 2;
  
  // 仅使用有效的延迟值（正值）
  if (delay > 0) {
    // 可以根据需要添加延迟过滤逻辑
    printf("NTP Delay: %lld ns\n", delay / 4294); // 转换为纳秒（近似值）
  }
  
  // 应用时间偏差补偿
  local_ntp_timestamp = timestamp + offset;
  
  // 减去1900年到1970年的时间差
  local_ntp_timestamp -= DIFF_SEC_1900_1970;
  
  // 考虑时区
  local_ntp_timestamp += SEC_TIME_ZONE;
  
  // 转换为可读格式（这里需要处理64位转time_t的问题，可能需要根据平台调整）
  time_t time_sec = (time_t)(local_ntp_timestamp >> 32); // 只使用秒部分
  synchronized_local_time = gmtime(&time_sec);
}

static void ntp_request(void)
{
  struct netconn * conn= NULL;
  struct netbuf * buf = NULL;
  uint8_t   ntp_request_buf[NTP_PKT_LEN];
  uint8_t*  ntp_receive_buf_p;
  uint16_t buf_len;
  err_t err;
  //get ntp server address
#if USE_DNS
  if(netconn_gethostbyname(ntp_server_list[0],&ntp_server_addr)!=ERR_OK)
  {
    IP4_ADDR(&ntp_server_addr, NTP_Server_ADDR0, NTP_Server_ADDR1, NTP_Server_ADDR2, NTP_Server_ADDR3); 
  }
#else
  IP4_ADDR(&ntp_server_addr, NTP_Server_ADDR0, NTP_Server_ADDR1, NTP_Server_ADDR2, NTP_Server_ADDR3);
#endif
  
    //Create new netconn
  conn = netconn_new(NETCONN_UDP);
  
  if(conn!= NULL)
  {
    buf = netbuf_new();
    if(buf != NULL)
    {
        //initialize ntp packet to 0
        memset(ntp_request_buf, 0, NTP_PKT_LEN);
        //buid ntp packet
        ntp_request_buf[0] = NTP_LI_NO_WARNING|NTP_VERSION|NTP_MODE_CLIENT;
       
        err = netbuf_ref(buf,ntp_request_buf,NTP_PKT_LEN);
        if(err ==ERR_OK)
        {
            //connect to NTP server
            err = netconn_connect(conn,&ntp_server_addr,NTP_PORT);
            if(err == ERR_OK)
            {           
                //send ntp request to ntp server
                if(netconn_send(conn,buf)!=ERR_OK)
                {
  
                }
                netbuf_delete( buf );
                //reveive ntp response
                netconn_recv(conn,&buf);
                            
                netbuf_data(buf,(void**)&ntp_receive_buf_p,&buf_len);
                
                //check ntp packet
                if(buf_len==NTP_PKT_LEN)
                {
                  if(((ntp_receive_buf_p[0]& NTP_MODE_MASK) == NTP_MODE_SERVER)|| ((ntp_receive_buf_p[0] & NTP_MODE_MASK) == NTP_MODE_BROADCAST))
                  {
                    // extract time from packet
                    // 在发送请求前记录T1时间戳
                    Org_Timestamp = get_system_time_as_ntp_timestamp();
                    
                    // 提取服务器返回的64位时间戳
                    // T2: Receive Timestamp
                    Receive_Timestamp = ((uint64_t)ntp_receive_buf_p[RECEIVE_TS_OFFSET] << 56) |
                    ((uint64_t)ntp_receive_buf_p[RECEIVE_TS_OFFSET+1] << 48) |
                    ((uint64_t)ntp_receive_buf_p[RECEIVE_TS_OFFSET+2] << 40) |
                    ((uint64_t)ntp_receive_buf_p[RECEIVE_TS_OFFSET+3] << 32) |
                    ((uint64_t)ntp_receive_buf_p[RECEIVE_TS_OFFSET+4] << 24) |
                    ((uint64_t)ntp_receive_buf_p[RECEIVE_TS_OFFSET+5] << 16) |
                    ((uint64_t)ntp_receive_buf_p[RECEIVE_TS_OFFSET+6] << 8) |
                    ((uint64_t)ntp_receive_buf_p[RECEIVE_TS_OFFSET+7]);
                    
                    // T3: Transmit Timestamp
                    Transmit_Timestamp = ((uint64_t)ntp_receive_buf_p[TRANSMIT_TS_OFFSET] << 56) |
                    ((uint64_t)ntp_receive_buf_p[TRANSMIT_TS_OFFSET+1] << 48) |
                    ((uint64_t)ntp_receive_buf_p[TRANSMIT_TS_OFFSET+2] << 40) |
                    ((uint64_t)ntp_receive_buf_p[TRANSMIT_TS_OFFSET+3] << 32) |
                    ((uint64_t)ntp_receive_buf_p[TRANSMIT_TS_OFFSET+4] << 24) |
                    ((uint64_t)ntp_receive_buf_p[TRANSMIT_TS_OFFSET+5] << 16) |
                    ((uint64_t)ntp_receive_buf_p[TRANSMIT_TS_OFFSET+6] << 8) |
                    ((uint64_t)ntp_receive_buf_p[TRANSMIT_TS_OFFSET+7]);
                    
                    // 记录T4时间戳
                    Destination_Timestamp = get_system_time_as_ntp_timestamp();
                    // start conver time format
                    ntp_process(Transmit_Timestamp);
                  }
                }
                
                netconn_close(conn);
                netconn_delete(conn); 
                netbuf_delete( buf );
            }
            else
            {
              netconn_delete(conn); 
              netbuf_delete( buf ); 
            }
        }
        else//
        {
          netconn_delete(conn); 
           netbuf_delete( buf );
           
        }
    }
    else //buf ==NULL
    {
      netconn_delete(conn);
    }
  }
  //process
}

static void ntp_thread(void *arg)
{
  while(1)
  {
    /*#-send ntp request ##############################*/
    ntp_request();
    
    /*#-update local time #############################*/
    printf("RTC time before update\n");
    RTC_CalendarShow();
    
    RTC_CalendarUpdate(synchronized_local_time);
    
    /*display local time*/
    printf("RTC time after update\n");
    RTC_CalendarShow();
    
    /*#-delay 1min ######################################*/
    osDelay(60*1000);
  }
  
}

void ntp_client_init(void)
{
  sys_thread_new("ntp_thread", ntp_thread, NULL, DEFAULT_THREAD_STACKSIZE, NTP_THREAD_PRIO);
}