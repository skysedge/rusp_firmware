#!/usr/bin/python

import RPi.GPIO as GPIO
import time
import subprocess
import serial
import threading

#3 5 7 11
K1_2560 = 5     
K2_2560 = 11     
K1_16U2 = 3     
K2_16U2 = 7     
prog = 36        #Orange
progtest = 38        #Yellow
test = 40        #Brown

GPIO.setmode(GPIO.BOARD)          #Sets pin numbering scheme (BOARD vs BCM)
GPIO.setup(K1_2560, GPIO.OUT)   
GPIO.setup(K2_2560, GPIO.OUT)   
GPIO.setup(K1_16U2, GPIO.OUT)   
GPIO.setup(K2_16U2, GPIO.OUT)   
GPIO.setup(prog, GPIO.IN, pull_up_down=GPIO.PUD_UP)   
GPIO.setup(progtest, GPIO.IN, pull_up_down=GPIO.PUD_UP)   
GPIO.setup(test, GPIO.IN, pull_up_down=GPIO.PUD_UP)   

def flash16u2():
    print('Switching to 16U2')
    GPIO.output(K1_16U2, GPIO.HIGH) 
    GPIO.output(K2_16U2, GPIO.HIGH)
    time.sleep(.1) 
    GPIO.output(K1_16U2, GPIO.LOW) 
    GPIO.output(K2_16U2, GPIO.LOW)
    time.sleep(.5)            
    print('Flashing 16U2')
    result = subprocess.run(["make", "u2"], check=True)
    print(result.stdout)
    time.sleep(1)

def flashbootloader():
    print('Switching to 2560')
    GPIO.output(K1_2560, GPIO.HIGH) 
    GPIO.output(K2_2560, GPIO.HIGH)
    time.sleep(.1)             
    GPIO.output(K1_2560, GPIO.LOW) 
    GPIO.output(K2_2560, GPIO.LOW)    
    time.sleep(.5)            
    print('Flashing 2560 bootloader')
    result = subprocess.run(["make", "bootloader"], check=True)
    print(result.stdout)
    time.sleep(1)

def flashrusp():
    print('Flashing RUSP firmware')
    result = subprocess.run(["make", "usb"], check=True)
    print(result.stdout)
    time.sleep(1)

def serialtests():
    # Open the serial connection
    ser = serial.Serial('/dev/ttyACM0', 115200, timeout=0.1)

    # Flag to control the reader thread
    running = True

    def read_serial():
        """Continuously read and display messages from the device"""
        while running:
            if ser.in_waiting:
                response = ser.readline().decode('utf-8', errors='ignore').strip()
                if response:
                    print(f"Device: {response}")
            time.sleep(0.01)

    # Start the reader thread
    reader_thread = threading.Thread(target=read_serial, daemon=True)
    reader_thread.start()

    def send_command(cmd):
        print(f"Sending: {cmd}")
        bytes_written = ser.write(f"{cmd}\r".encode())
        print(f"  Wrote {bytes_written} bytes")
        time.sleep(0.5)  # Wait longer for response

    # Wait for startup messages
    print("Waiting for startup messages...")
    time.sleep(12)

    # Send your commands
    send_command("at")
    time.sleep(1)
    send_command("AT+UEXTDCONF=0,1") #CODEC first time configuration
    time.sleep(2)
    send_command("AT+CFUN=16") #Reset modem
    time.sleep(10)
    print("sending tone to speaker in 3")
    time.sleep(1)
    print("2")
    time.sleep(1)
    print("1")
    time.sleep(1)
    send_command("AT+UTGN=1000,1000,100,0") #make tone

    # Give more time for final responses
    time.sleep(2)

    # Clean up
    running = False
    reader_thread.join(timeout=1)
    ser.close()

print('Cycling relays')
GPIO.output(K1_2560, GPIO.HIGH) 
GPIO.output(K2_2560, GPIO.HIGH)
time.sleep(.1)             

GPIO.output(K1_2560, GPIO.LOW) 
GPIO.output(K2_2560, GPIO.LOW)    
time.sleep(.5)

GPIO.output(K1_16U2, GPIO.HIGH) 
GPIO.output(K2_16U2, GPIO.HIGH)
time.sleep(.1) 

GPIO.output(K1_16U2, GPIO.LOW) 
GPIO.output(K2_16U2, GPIO.LOW)
time.sleep(.5)            
print('Ready')

while True:
    try:
        if GPIO.input(prog) == False:
            flash16u2()
            flashbootloader()
            flashrusp()
            print('Finished')

        if GPIO.input(progtest) == False:
            flash16u2()
            flashbootloader()
            flashrusp()
            serialtests()
            print('Finished')

        if GPIO.input(test) == False:
            serialtests()
            print('Finished')


    except KeyboardInterrupt: #looks for ctrl+C
        print("Program terminated by user.")
        GPIO.output(K1_2560, GPIO.LOW) 
        GPIO.output(K2_2560, GPIO.LOW)    
        GPIO.output(K1_16U2, GPIO.LOW) 
        GPIO.output(K2_16U2, GPIO.LOW)
        GPIO.cleanup() # Clean up GPIO settings
        break



