A personal project to find a solution to a niche problem. I drive a 2008 Infiniti G35 which has steering wheel controls for the radio and CD player but is too old for Bluetooth audio. I connect my phone with a USB-C to 3.5 adapter into the Aux input but had no way to quickly change songs. I had the idea to tap into the headunit and read the button presses and pass them along to an ESP32 connected to my phone over Bluetooth.

The steering wheel buttons are connected over a resistor ladder. Looking in the factory service manual I was able to find the voltage values when the UP and DOWN button are pressed. On my car UP was 0.7v and DOWN was 1.3v. After wiring everything up I got slighly different values but not too far off. The idle voltage was reading around 2.25v and DOWN was 1.5v.

I put together an op-amp circuit with some filtering to help stabilize the ADC readings from the buttons in the car. This way allows the buttons still function like normal in other modes and not load down the line. I am not a coder so I got ChatGPT to do a lot of the heavy lifting. The ESP32 connects to the phone as a HID and is able to map the incoming ADC values to next and previous track commands for Spotify (or any music app).

![PXL_20251108_194223257](https://github.com/user-attachments/assets/d24c21ab-730d-4a4d-9f6d-e714c6a87606)
