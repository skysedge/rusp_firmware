#!/usr/bin/python

import RPi.GPIO as GPIO
import time

#3 5 7 11
K1_2560 = 5     
K2_2560 = 11     #!!
K1_16U2 = 3     
K2_16U2 = 7     #!!
SW2 = 36        #Orange
SW3 = 38        #Yellow
SW4 = 40        #Brown

GPIO.setmode(GPIO.BOARD)          #Sets pin numbering scheme (BOARD vs BCM)
GPIO.setup(K1_2560, GPIO.OUT)   
GPIO.setup(K2_2560, GPIO.OUT)   
GPIO.setup(K1_16U2, GPIO.OUT)   
GPIO.setup(K2_16U2, GPIO.OUT)   
GPIO.setup(SW2, GPIO.IN, pull_up_down=GPIO.PUD_UP)   
GPIO.setup(SW3, GPIO.IN, pull_up_down=GPIO.PUD_UP)   
GPIO.setup(SW4, GPIO.IN) 

while True:
    try:
        if GPIO.input(SW2) == False:
            time.sleep(.5)
            print('Switching to 2560')
            GPIO.output(K1_2560, GPIO.HIGH) 
            GPIO.output(K2_2560, GPIO.HIGH)
            time.sleep(.1)             

            GPIO.output(K1_2560, GPIO.LOW) 
            GPIO.output(K2_2560, GPIO.LOW)    

        if GPIO.input(SW3) == False:
            print('Switching to 16U2')
            time.sleep(.5)
            GPIO.output(K1_16U2, GPIO.HIGH) 
            GPIO.output(K2_16U2, GPIO.HIGH)
            time.sleep(.1) 

            GPIO.output(K1_16U2, GPIO.LOW) 
            GPIO.output(K2_16U2, GPIO.LOW)
            time.sleep(.5)            

    except KeyboardInterrupt: #looks for ctrl+C
        print("Program terminated by user.")
        GPIO.cleanup() # Clean up GPIO settings
        break



