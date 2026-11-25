# FoxFW_subghz_plus
Modified version of flipperzero FoxFW with some fixes, like correct buttons, rolling code counters and protocol additions from KAT database. Fixes (counter and button mapping) were tested with Hackrf One and KAT. Of course, there are still bugs, like some protocols, ex. Toyota Lexus does not save correctly etc. Also it is very messy, because I did not want to waste tokens on externalization, so it takes lot of flash space and allows for less protocols, most of disabled are mostly American, change this in lib/subghz/protocols/protocol_items.c, some of the info is also in it.
**This is not official FW version, only my mod with some subghz protocols not present in any FW till now, so somebody does not have to waste more tokens on this**

---
> [!WARNING]
> As always, test this version or official only on systems and vehicles you own or have explicit, written permission to test. Capturing, decoding, or transmitting keyfob and vehicle-access signals without authorization may be illegal in your jurisdiction (e.g. computer misuse, unauthorized access, or radio regulations). You are solely responsible for ensuring your use complies with all applicable laws. The authors and contributors assume no liability for misuse or damage arising from use of this software.
---

## **Credits**
- **KaraZajac** for her software - [KAT](https://github.com/KaraZajac/KAT/tree/main) and amazing keyfob research website for it: https://kat.karazajac.io/
- **thedoctorz** for his firmware, mainly some subghz protocols - [DoctorFW](https://github.com/thedoctorz/doctor-firmware) 
- **Michael Joyce and Adam Alioa** for their [FoxFW](https://github.com/FoxFW/flipperzero-firmware), really interesting idea about externalization, that I have never seen before in flipper firmware
- Ideas for fixes by me **@bababooex** and realised with **Claude**, otherwise it would take me ages
