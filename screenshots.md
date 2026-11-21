# Tab5 v5.4 screenshot gallery

## Startup and setup

![Startup Settings on the Tab5](screenshots/Tab5-Startup-Settings.png)

**Startup Settings.** Choose a Macintosh hard disk, optional CD-ROM and memory size, then select **Start Up**.

![Set Up SD Card with Mac OS 8.1 and System 7 choices](screenshots/Tab5-Setup.png)

**Set Up SD Card.** Connect to Wi-Fi and choose Mac OS 8.1 or System 7. Setup downloads the Macintosh ROM and the selected startup disk. Allow 1.2 GiB of free space on the card.

## Download, installation and verification

![Mac OS 8.1 download showing progress and network speed](screenshots/Tab5-Starter-Download.png)

**Downloading.** This Tab5 capture shows the Mac OS 8.1 download at 273.2 KiB/s. An interrupted download can resume from the bytes saved on the card.

![Mac OS 8.1 installation progress at 65%](screenshots/Tab5-Installing.png)

**Installing.** The original v5.4 renderer drew this illustration from a recorded Tab5 progress sample. Setup had expanded and written 687,407,104 of 1,048,576,000 bytes to `Macintosh8.dsk` (65%).

![The Tab5 verifying a startup file on the microSD card](screenshots/Tab5-Imaging.png)

**Verifying.** Setup checks the compressed download, expands the disk image onto the card, then rereads and verifies the installed image. The network speed appears only during the download.

## Card management

![SD Card inspection showing the card and its mounted volume](screenshots/Tab5-SD-Card.png)

**SD Card.** Check the card identity, physical capacity, filesystem and volume size before choosing a card action.

![Erase SD Card confirmation on the Tab5](screenshots/Tab5-Format-Confirm.png)

**Erase SD Card?** The confirmation names the card. **Erase Card** removes every file and partition and creates a FAT32 volume. **Cancel** returns to inspection.

![USB Disk mode on the Tab5](screenshots/Tab5-USB-Disk.png)

**USB Disk.** Connect a computer to the Tab5’s USB-A port to access the card. Eject the mounted card on the computer before leaving USB Disk mode.

## Finder and disk performance

![Macintosh Finder running on the Tab5](screenshots/Tab5-Finder.png)

**Finder.** The Macintosh desktop after startup on the Tab5.

![The Tab5 Wi-Fi console in a browser](screenshots/Tab5-Web-Console.png)

**Wi-Fi console.** Open the console in a browser to view the Macintosh display, check performance or send mouse and keyboard input. [Wi-Fi console instructions](AUTOMATION.md#wi-fi-console)

![The System 7 guest disk test showing verified reads and writes](screenshots/Tab5-System7-Disk-Benchmark.png)

**Guest disk test.** The benchmark writes 4 MiB to the Macintosh startup volume, reads it back, verifies every byte and removes its test file. [Four-bit SDMMC and SPI measurements](SD_STABILITY.md)

## Touch input

| QWERTY keyboard on Tab5 | Game controls on Tab5 |
| --- | --- |
| ![v4 photo of the Tab5 keyboard overlay](screenshots/v4-M5Keyboard.jpeg) | ![v4 photo of the Tab5 gaming D-pad overlay](screenshots/v4-M5Gamemode.jpeg) |
| Three-finger tap to show or hide the keyboard. | Four-finger tap to show or hide the D-pad. |

The photos above show the v4 controls on a physical Tab5. [Keyboard on the Waveshare 10.1-inch display](screenshots/v4-WaveshareBootKeyboard.jpeg)

## Earlier releases

<details>
<summary>Boot recordings and Flying Toasters</summary>

![System 7.5.3 starting up on the Tab5](screenshots/MacOS753.gif)

**System 7.5.3 startup.** A recording of the physical display. [Boot video (MOV)](https://github.com/amcchord/M5Tab-Macintosh/blob/v5.4/screenshots/MacOS753_Booting.mov)

![Flying Toasters on the Tab5 with v2.5 firmware](screenshots/Toasters2.5.gif)

**Flying Toasters.** After Dark running on the Tab5 in v2.5. [Original recording (GIF)](screenshots/Toasters.gif)

</details>

<details>
<summary>Hardware photos, benchmarks and web browsing</summary>

| Mac OS 8.1 startup on the Tab5 | Mac OS 8.1 Finder on the Tab5 |
| --- | --- |
| ![Tab5 displaying the Mac OS startup screen](screenshots/MacOS8.1_Booting.jpeg) | ![Tab5 displaying the Mac OS 8.1 desktop](screenshots/MacOS8.1_Booted.jpeg) |

**About This Computer.** [Mac OS 8.1 photo](screenshots/MacOS8.1_About.jpeg) and [System 7.5.3 photo](screenshots/MacOS753_About.jpeg).

**Speedometer.** The Performance Test window photographed on the Tab5 in [v2.5](screenshots/2-5-Performance.jpeg) and [v3.0](screenshots/3-0-Performance.png).

![System 7 Today open in Internet Explorer on the Tab5](screenshots/Internet-Sys7.jpg)

**Web browsing over Wi-Fi.** Internet Explorer on System 7 displaying the System 7 Today site. [Forum photo](screenshots/Internert-Forums.jpg) · [Page load video (MP4)](https://github.com/amcchord/M5Tab-Macintosh/blob/v5.4/screenshots/LoadingAWebpage.mp4)

</details>
