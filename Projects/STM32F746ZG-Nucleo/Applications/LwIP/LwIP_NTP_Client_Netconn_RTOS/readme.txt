LwIP_NTP_Client_Netconn_RTOS Application
  
This demo is aimed to show how to synchronize system time from internet by NTP protocol.This demo doesn't implement complex algrithm,just use the feedback from one server to update local time.

This demo use DNS to get the IP address of NTP server, if it fails,a staic IP address will be used.

Configure NTP server in ntp_server_list[].

When get response from NTP server, use Transmit_Timestamp to update RTC

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~


