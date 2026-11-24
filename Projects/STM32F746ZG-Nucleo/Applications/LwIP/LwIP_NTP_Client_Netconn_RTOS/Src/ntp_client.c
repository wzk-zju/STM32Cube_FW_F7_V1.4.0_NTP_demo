#include "ntp_client.h"
#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/api.h"

#define NTP_THREAD_PRIO       (tskIDLE_PRIORITY + 4)
#define NTP_RECV_TIMEOUT_MS   3000
#define NTP_SECONDS_2000      3155673600UL

static const char *ntp_server = "1.cn.pool.ntp.org";
static ip_addr_t ntp_server_addr;

extern void RTC_CalendarShow(void);
extern int RTC_CalendarUpdate(const struct tm *newtime);
extern int RTC_GetTime(struct tm *localtime, uint32_t *fraction);

static int ntp_is_leap_year(int year)
{
  return (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0));
}

static int ntp_days_in_month(int year, int month)
{
  static const uint8_t days[] = {31, 28, 31, 30, 31, 30,
                                 31, 31, 30, 31, 30, 31};
  if (month == 1 && ntp_is_leap_year(year)) {
    return 29;
  }
  return days[month];
}

/* The RTC stores local time. The NTP offset calculation corrects its
 * arbitrary initial offset; the server supplies UTC. */
static int ntp_get_rtc_timestamp(uint64_t *timestamp)
{
  struct tm calendar;
  uint32_t fraction;
  uint32_t days = 0;
  uint64_t seconds;
  int year;
  int month;

  if (!RTC_GetTime(&calendar, &fraction)) {
    return 0;
  }
  year = calendar.tm_year + 1900;
  if (year < 2000 || year > 2099 || calendar.tm_mon < 0 || calendar.tm_mon > 11 ||
      calendar.tm_mday < 1 || calendar.tm_mday > ntp_days_in_month(year, calendar.tm_mon) ||
      calendar.tm_hour < 0 || calendar.tm_hour > 23 ||
      calendar.tm_min < 0 || calendar.tm_min > 59 ||
      calendar.tm_sec < 0 || calendar.tm_sec > 59) {
    return 0;
  }

  for (year = 1970; year < calendar.tm_year + 1900; ++year) {
    days += ntp_is_leap_year(year) ? 366U : 365U;
  }
  for (month = 0; month < calendar.tm_mon; ++month) {
    days += (uint32_t)ntp_days_in_month(year, month);
  }
  days += (uint32_t)calendar.tm_mday - 1U;
  seconds = (uint64_t)days * 86400U + (uint32_t)calendar.tm_hour * 3600U +
            (uint32_t)calendar.tm_min * 60U + (uint32_t)calendar.tm_sec +
            DIFF_SEC_1900_1970;
  *timestamp = ((uint64_t)(uint32_t)seconds << 32) | fraction;
  return 1;
}

/* Expand the NTP seconds era for the RTC's supported years (2000-2099). */
static int ntp_timestamp_to_local_calendar(uint64_t timestamp, struct tm *calendar)
{
  uint32_t seconds32;
  uint64_t seconds;
  uint32_t days;
  uint32_t day_seconds;
  uint32_t year_days;
  int year = 1970;
  int month = 0;

  timestamp += (uint64_t)SEC_TIME_ZONE << 32;
  seconds32 = (uint32_t)(timestamp >> 32);
  seconds = seconds32;
  if (seconds32 < NTP_SECONDS_2000) {
    seconds += 1ULL << 32; /* NTP era 1 starts in 2036. */
  }
  if (seconds < DIFF_SEC_1900_1970) {
    return 0;
  }
  seconds -= DIFF_SEC_1900_1970;
  days = (uint32_t)(seconds / 86400U);
  day_seconds = (uint32_t)(seconds % 86400U);

  memset(calendar, 0, sizeof(*calendar));
  calendar->tm_wday = (int)((days + 4U) % 7U);
  while (year <= 2099) {
    year_days = ntp_is_leap_year(year) ? 366U : 365U;
    if (days < year_days) {
      break;
    }
    days -= year_days;
    ++year;
  }
  if (year < 2000 || year > 2099) {
    return 0;
  }
  calendar->tm_year = year - 1900;
  calendar->tm_yday = (int)days;
  while (days >= (uint32_t)ntp_days_in_month(year, month)) {
    days -= (uint32_t)ntp_days_in_month(year, month);
    ++month;
  }
  calendar->tm_mon = month;
  calendar->tm_mday = (int)days + 1;
  calendar->tm_hour = (int)(day_seconds / 3600U);
  calendar->tm_min = (int)((day_seconds % 3600U) / 60U);
  calendar->tm_sec = (int)(day_seconds % 60U);
  calendar->tm_isdst = 0;
  return 1;
}

static void ntp_write_timestamp(uint8_t *field, uint64_t timestamp)
{
  int i;
  for (i = 7; i >= 0; --i) {
    field[i] = (uint8_t)timestamp;
    timestamp >>= 8;
  }
}

static uint64_t ntp_read_timestamp(const uint8_t *field)
{
  uint64_t timestamp = 0;
  int i;
  for (i = 0; i < 8; ++i) {
    timestamp = (timestamp << 8) | field[i];
  }
  return timestamp;
}

/* Interpret a wrapped 32.32 subtraction when the clocks differ by <68 years. */
static int ntp_signed_difference(uint64_t left, uint64_t right, int64_t *result)
{
  uint64_t difference = left - right;
  if (difference <= (uint64_t)INT64_MAX) {
    *result = (int64_t)difference;
    return 1;
  }
  difference = right - left;
  if (difference <= (uint64_t)INT64_MAX) {
    *result = -(int64_t)difference;
    return 1;
  }
  return 0;
}

/* Return success only when this response has actually updated the RTC. */
static int ntp_request(void)
{
  struct netconn *conn = NULL;
  struct netbuf *send_buf = NULL;
  struct netbuf *recv_buf = NULL;
  struct tm local_time;
  uint8_t request[NTP_PKT_LEN] = {0};
  uint8_t response[NTP_PKT_LEN];
  uint64_t t1, t2, t3, t4, corrected;
  int64_t server_minus_local;
  uint32_t receive_tick;
  int success = 0;

#if USE_DNS
  if (netconn_gethostbyname(ntp_server, &ntp_server_addr) != ERR_OK) {
    IP4_ADDR(&ntp_server_addr, NTP_Server_ADDR0, NTP_Server_ADDR1,
             NTP_Server_ADDR2, NTP_Server_ADDR3);
  }
#else
  IP4_ADDR(&ntp_server_addr, NTP_Server_ADDR0, NTP_Server_ADDR1,
           NTP_Server_ADDR2, NTP_Server_ADDR3);
#endif

  conn = netconn_new(NETCONN_UDP);
  if (conn == NULL) {
    goto cleanup;
  }
  netconn_set_recvtimeout(conn, NTP_RECV_TIMEOUT_MS);
  if (netconn_connect(conn, &ntp_server_addr, NTP_PORT) != ERR_OK) {
    goto cleanup;
  }
  send_buf = netbuf_new();
  if (send_buf == NULL || !ntp_get_rtc_timestamp(&t1)) {
    goto cleanup;
  }
  request[0] = NTP_LI_NO_WARNING | NTP_VERSION | NTP_MODE_CLIENT;
  ntp_write_timestamp(&request[TRANSMIT_TS_OFFSET], t1);
  if (netbuf_ref(send_buf, request, sizeof(request)) != ERR_OK ||
      netconn_send(conn, send_buf) != ERR_OK) {
    goto cleanup;
  }
  netbuf_delete(send_buf);
  send_buf = NULL;

  if (netconn_recv(conn, &recv_buf) != ERR_OK || recv_buf == NULL) {
    goto cleanup;
  }
  /* Record T4 before parsing or printing the response. */
  if (!ntp_get_rtc_timestamp(&t4)) {
    goto cleanup;
  }
  receive_tick = HAL_GetTick();
  if (netbuf_len(recv_buf) < NTP_PKT_LEN ||
      netbuf_copy(recv_buf, response, NTP_PKT_LEN) != NTP_PKT_LEN ||
      (response[0] & NTP_MODE_MASK) != NTP_MODE_SERVER ||
      ((response[0] >> 6) & 3U) == 3U ||
      ((response[0] >> 3) & 7U) < 3U ||
      ((response[0] >> 3) & 7U) > 4U ||
      response[1] == 0 || response[1] > 15U ||
      ntp_read_timestamp(&response[ORIGINATE_TS_OFFSET]) != t1) {
    goto cleanup;
  }
  t2 = ntp_read_timestamp(&response[RECEIVE_TS_OFFSET]);
  t3 = ntp_read_timestamp(&response[TRANSMIT_TS_OFFSET]);
  if (t2 == 0 || t3 == 0) {
    goto cleanup;
  }
  /* NTP offset: ((T2 - T1) + (T3 - T4)) / 2. Split the terms to
   * avoid overflowing a signed 64-bit sum during initial synchronization. */
  {
    int64_t a;
    int64_t b;
    if (!ntp_signed_difference(t2, t1, &a) ||
        !ntp_signed_difference(t3, t4, &b)) {
      goto cleanup;
    }
    server_minus_local = a / 2 + b / 2 + (a % 2 + b % 2) / 2;
  }
  corrected = t4 + (uint64_t)server_minus_local;
  /* Advance the received time to the moment of the RTC write. */
  corrected += ((uint64_t)(HAL_GetTick() - receive_tick) << 32) / 1000U;
  if (!ntp_timestamp_to_local_calendar(corrected, &local_time)) {
    goto cleanup;
  }
  success = RTC_CalendarUpdate(&local_time);

cleanup:
  if (recv_buf != NULL) {
    netbuf_delete(recv_buf);
  }
  if (send_buf != NULL) {
    netbuf_delete(send_buf);
  }
  if (conn != NULL) {
    netconn_delete(conn);
  }
  return success;
}

static void ntp_thread(void *arg)
{
  (void)arg;
  while (1) {
    if (ntp_request()) {
      printf("RTC updated from NTP\n");
      RTC_CalendarShow();
    } else {
      printf("NTP request failed; RTC was not updated\n");
    }
    osDelay(60 * 1000);
  }
}

void ntp_client_init(void)
{
  sys_thread_new("ntp_thread", ntp_thread, NULL, DEFAULT_THREAD_STACKSIZE,
                 NTP_THREAD_PRIO);
}
