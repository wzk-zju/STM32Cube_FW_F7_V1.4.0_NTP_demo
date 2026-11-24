LwIP_NTP_Client_Netconn_RTOS Application
  
This STM32F746ZG Nucleo demo synchronizes its RTC with one NTP server over UDP port 123. It is a simple client, not a full NTP clock discipline implementation.

The client resolves the server name with DNS and falls back to the static IP address in ntp_client.h if DNS fails.

Configure the server name in ntp_client.c and the fallback IP in ntp_client.h.

The client sends T1, reads T2/T3 and records T4, then calculates the clock offset. It validates the response before updating the RTC. Failed requests leave the RTC unchanged and are retried after 60 seconds. Receive timeout is 3 seconds.

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~


