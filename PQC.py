#!/usr/bin/python

import RPi.GPIO as GPIO
import time
import subprocess
import serial
import threading
import glob

#3 5 7 11
K1_2560 = 5     
K2_2560 = 11     
K1_16U2 = 3     
K2_16U2 = 7     
prog1 = 36        #Orange
prog2 = 38        #Yellow
test = 40        #Brown

GPIO.setmode(GPIO.BOARD)          #Sets pin numbering scheme (BOARD vs BCM)
GPIO.setup(K1_2560, GPIO.OUT)   
GPIO.setup(K2_2560, GPIO.OUT)   
GPIO.setup(K1_16U2, GPIO.OUT)   
GPIO.setup(K2_16U2, GPIO.OUT)   
GPIO.setup(prog1, GPIO.IN, pull_up_down=GPIO.PUD_UP)   
GPIO.setup(prog2, GPIO.IN, pull_up_down=GPIO.PUD_UP)   
GPIO.setup(test, GPIO.IN, pull_up_down=GPIO.PUD_UP)   

def find_serial_port():
    """Find the first available /dev/ttyACM* port"""
    ports = glob.glob('/dev/ttyACM*')
    if ports:
        ports.sort()  # Use the lowest numbered port
        print(f"Found serial port: {ports[0]}")
        return ports[0]
    else:
        print("ERROR: No /dev/ttyACM* ports found")
        return None

def makeu2():
    print('Switching to 16U2')
    GPIO.output(K1_16U2, GPIO.HIGH) 
    GPIO.output(K2_16U2, GPIO.HIGH)
    time.sleep(.1) 
    GPIO.output(K1_16U2, GPIO.LOW) 
    GPIO.output(K2_16U2, GPIO.LOW)
    time.sleep(1)            
    print('Flashing 16U2')
    result = subprocess.run(["make", "u2"])
    if result.returncode != 0:
        print(f"ERROR: Failed to flash 16U2 - check connections")
        return False
    print("16U2 flashed successfully")
    time.sleep(1)
    return True

def makebootloader():
    print('Switching to 2560')
    GPIO.output(K1_2560, GPIO.HIGH) 
    GPIO.output(K2_2560, GPIO.HIGH)
    time.sleep(.1)             
    GPIO.output(K1_2560, GPIO.LOW) 
    GPIO.output(K2_2560, GPIO.LOW)    
    time.sleep(1)            
    print('Flashing 2560 bootloader')
    result = subprocess.run(["make", "bootloader"])
    if result.returncode != 0:
        print(f"ERROR: Failed to flash 2560 bootloader - check connections")
        return False
    print("2560 bootloader flashed successfully")
    time.sleep(1)
    return True

def makeusb():
    # Find the port before programming
    port = find_serial_port()
    if not port:
        print("ERROR: No serial port found - device must be connected")
        return False
    
    print(f'Flashing RUSP firmware using {port}')
    result = subprocess.run(["make", "usb", f"PORT={port}"])
    if result.returncode != 0:
        print(f"ERROR: Failed to flash RUSP firmware - check connections")
        return False
    print("RUSP firmware flashed successfully")
    print("Waiting for device to re-enumerate...")
    time.sleep(3)  # Give device time to reset and reconnect
    
    # Verify the port is available
    port = find_serial_port()
    if port:
        print(f"Device ready on {port}")
    else:
        print("Warning: No serial port detected after programming")
    
    return True

def serialtests():
    """Full test sequence including first-time CODEC configuration"""
    # Find and open the serial connection
    port = find_serial_port()
    if not port:
        print("Check that the device is connected and enumerated properly")
        return False
    
    try:
        ser = serial.Serial(port, 115200, timeout=0.1,
                           xonxoff=False,    # Disable software flow control
                           rtscts=False,     # Disable hardware (RTS/CTS) flow control  
                           dsrdtr=False)     # Disable hardware (DSR/DTR) flow control
        print(f"Opened {port} with settings: 115200 8N1, no flow control")
    except serial.SerialException as e:
        print(f"ERROR: Could not open {port}: {e}")
        return False

    # Flag to control the reader thread
    running = True

    def read_serial():
        """Continuously read and display messages from the device"""
        while running:
            try:
                if ser.in_waiting:
                    response = ser.readline().decode('utf-8', errors='ignore').strip()
                    if response:
                        print(f"Device: {response}")
                time.sleep(0.01)
            except (OSError, serial.SerialException) as e:
                print(f"Serial read error: {e}")
                break

    # Start the reader thread
    reader_thread = threading.Thread(target=read_serial, daemon=True)
    reader_thread.start()

    def send_command(cmd):
        command_bytes = f"{cmd}\r".encode()
        print(f"Sending: {cmd}")
        print(f"  Bytes (hex): {command_bytes.hex()}")
        bytes_written = ser.write(command_bytes)
        ser.flush()  # Ensure data is sent
        print(f"  Wrote {bytes_written} bytes")
        time.sleep(2.5)  # Increased to give more time for response

    # Wait for startup messages
    print("Waiting for startup messages...")
    time.sleep(20)

    # Send your commands
    send_command("AT")
    time.sleep(10)  # Longer delay needed before CODEC config command
    # NOTE: This command needs significant delay after the first AT command
    # Shorter delays (2-5 sec) cause it to return ERROR instead of OK
    send_command("AT+UEXTDCONF=0,1")
    time.sleep(3)  # This command takes longer to process
    send_command("AT+CFUN=16") #Reset modem
    time.sleep(15)
    send_command("AT")
    time.sleep(1) 
    print("sending tone to speaker in 3")
    time.sleep(1)
    print("2")
    time.sleep(1)
    print("1")
    time.sleep(1)
    send_command("AT+UTGN=1000,1000,100,0") #make tone
    time.sleep(1)
    print("TURN OFF MB NOW")
    time.sleep(6)

    # Clean up
    running = False
    reader_thread.join(timeout=1)
    ser.close()
    return True


def reset_usb_device(vendor_id, product_id):
    """Reset USB device by toggling authorized flag"""
    # Find the device in sysfs
    pattern = f'/sys/bus/usb/devices/*/idVendor'
    for vendor_file in glob.glob(pattern):
        with open(vendor_file, 'r') as f:
            if f.read().strip() == vendor_id:
                device_dir = vendor_file.replace('/idVendor', '')
                product_file = f'{device_dir}/idProduct'
                with open(product_file, 'r') as f:
                    if f.read().strip() == product_id:
                        # Found it! Toggle authorized
                        auth_file = f'{device_dir}/authorized'
                        print(f"Resetting USB device at {device_dir}")
                        with open(auth_file, 'w') as f:
                            f.write('0')
                        time.sleep(0.5)
                        with open(auth_file, 'w') as f:
                            f.write('1')
                        time.sleep(2)
                        return True
    return False



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
        if GPIO.input(prog1) == False:
            if makeu2():
                if makebootloader():
                    print('COMPLETE. Reset MB in fixture and attach DB if not already done. POWER SWITCH ON.')
                else:
                    print('Programming failed at bootloader step')
            else:
                print('Programming failed at 16U2 step')

        if GPIO.input(prog2) == False:
            if makeusb(): 
                print('Finished flashing RUSP firmware via \'make usb\'.')
                serialtests()
                print('Finished serial tests. TURN POWER SWITCH OFF before removing.')
            else:
                print('Firmware programming failed - skipping serial tests')

        if GPIO.input(test) == False:
            serialtests()
            print('Finished serial tests. TURN POWER SWITCH OFF before removing.')


    except KeyboardInterrupt: #looks for ctrl+C
        print("Program terminated by user.")
        GPIO.output(K1_2560, GPIO.LOW) 
        GPIO.output(K2_2560, GPIO.LOW)  
        GPIO.output(K1_16U2, GPIO.LOW) 
        GPIO.output(K2_16U2, GPIO.LOW)
        GPIO.cleanup() # Clean up GPIO settings
        break
