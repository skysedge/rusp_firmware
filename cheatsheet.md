AT+CFUN=0                   power off
AT+CFUN=1                   power on
AT+CFUN=16                  silent reset with reset of SIM card

AT+CMEE=2                   enable verbose error reporting
AT+CREG=2                   enable network and location status reporting

AT+UCFSCAN=7                LTE tower scan

AT+COPS=?                   check towers we can register to
AT+COPS=1,0,"AT&T"          manually register with AT&T
AT+COPS=1,2,310410          manually register with MCC 310 MNC 410 (AT&T)

AT+CSQ                      signal strength
AT+CESQ                     signal strength
