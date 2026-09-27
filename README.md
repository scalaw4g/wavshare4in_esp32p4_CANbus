# Esp32 Round Display for a digital gauge cluster

This project is source-available and free for personal and educational use.
Commercial use is NOT permitted.

### *****Please do not use these files to sell this to others.  I made these so that those wanting to modify their dash/cluster dont have to spend an arm and a leg to do so.***** ###

Youtube Tutorial/playlist: https://youtu.be/t7H6pevep40

1. Install [visual studio code](https://code.visualstudio.com/Download)
2. Install the ESP-IDF extension(shown here: https://www.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-3.4C#Introduction_to_ESP-IDF_and_Environment_Setup_.28VSCode_Column.29)
    * Open VSCode Package Manager
    * Search for the official ESP-IDF ide extension
3. Open this repo folder in VS code
4. Plug in Esp32-P4/screen via usb c
5. Set target device to esp32p4(see "Description of Bottom Toolbar of VSCode User Interface" in the waveshare link above)
6. Build 
7. Flash
<br>


# CANBUS
For canbus integration, I have added a few CAN protocols(haltech, hondata, etc).  If you would like more added please join the discord and provide the can protocol and I can add it quickly
<br>
<br>
You will need to purchase a [small can transciever](https://a.co/d/09CiRq2o) for 9$.  With it, you can ignore any other sensor wiring.  If you need help with wiring those two wires, again join the discord.

<br>
<img width="3024" height="4032" alt="IMG_2690" src="https://github.com/user-attachments/assets/ff5cb604-1f43-4a8b-be12-6aa4c8036f45" />
<img width="3024" height="4032" alt="IMG_2689" src="https://github.com/user-attachments/assets/c69a84df-94ed-4d2f-b5ee-1fc4988e1a71" />
<img width="3024" height="4032" alt="IMG_2691" src="https://github.com/user-attachments/assets/67dda3b2-fb5f-41d8-b32d-e0fd29233b95" />


<br>At startup, tap **CAN FORWARD** on the boot screen to enter forwarding mode. If it is not tapped within 3.5 seconds, the dashboard starts. In forwarding mode, connect the PC to the board's USB Serial/JTAG port and select a Lawicel/SLCAN serial adapter in SavvyCAN or CANHacker. Set the application bitrate to match the detected CAN bus rate. The firmware streams received standard and extended CAN frames while the adapter is open; it does not run the dashboard or gauge UART outputs.

## Issues/bug fixes

### 4in waveshare round screens
this fork is already adapted for the 4in display version
<br>

