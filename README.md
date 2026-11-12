Linux 7.2.15 for the POCO X3 NFC (surya)
========================================

Mainline Linux for the Xiaomi POCO X3 NFC (codename surya, Snapdragon 732G / SM7150).
It runs the phone on **Mobian** (Debian sid with Mobian's staging repository) with GNOME Shell and
[gnome-phone](https://github.com/pulpoff/gnome-phone) (no Phosh). It is not a postmarketOS kernel: it is built
as a Debian package (`make bindeb-pkg`) and installed with dpkg like any other Debian kernel.

The tree is stable **v7.2.15**, plus the SM7150 work of
[sm7150-mainline/linux](https://github.com/sm7150-mainline/linux) (branch `v7.2`, brought up to 7.2.15 with
the stable incremental patches), plus the surya changes listed below.

7.2.15 is the last 7.2 stable release: kernel.org has marked 7.2 end of life, and this tree stays on it.
Use the `main` branch; the kernel release is `7.2.15-surya` (`LOCALVERSION=-surya`).

New in this version
===================

* **The phone sleeps on a computer's USB port.** The charger reported an input-limit change about three
  times a second there, and each report woke the phone out of a sleep. It now reports only a real change.
  See Suspend.
* **Short vibrations are felt.** A short effect drove the motor for a fraction of the time it asked for: a
  5 ms tick for under 1 ms. The motor now runs as long as it was asked to, and at least 15 ms. See
  Vibration.

The previous update (Linux 7.2.15, hardware video, 60 fps cameras, a cleaner boot picture) is commit
cb6904117b9d:

* **Linux 7.2.15.** 7.2.11 to 7.2.15 come from kernel.org's stable patches; none of them touches the
  surya changes below.
* **Hardware video decode and encode** (Venus, IRIS1): H.264, HEVC, VP8 and VP9 decode, H.264 and HEVC
  encode, so camera video records through the hardware encoder; and a fix for seeking in GStreamer
  players, which hung the decoder. See Video.
* **60 fps camera video**: 3840x2160 and 1920x1080 on the main camera, 2592x1940 and 1280x720 on the front
  camera, and the ultrawide's binned mode back at 60 fps. See Cameras.
* **A cleaner boot picture**: the hand-over from the boot logo to the boot splash no longer flashes white
  or goes black. See Touchscreen and display.
* **The top microphone works**: it had no SoundWire port of its own. See Audio.
* **Sleep:** suspend to idle works with the image's sleep daemon. What it takes and what is still open is
  under Suspend.

Changes for the POCO X3 NFC
===========================

Audio: WCD9375 codec, microphones and earpiece
----------------------------------------------

* **Device tree: WCD9375 microphone capture.** The codec sits on the LPASS SoundWire buses. Its
  reset is LPI GPIO 6. Supplies are L10A, L15A and BOB, and the three mic biases are 2700 mV, all
  as in Xiaomi's surya audio overlay. Capture gets its own MultiMedia2 frontend. `&q6asmdai dai@1`
  is added because q6asm only registers the DAIs listed there. It also gets a "WCD Capture" back
  end on TX_CODEC_DMA_TX_3 and the VA macro link. Built on the davinci work in sm7150-mainline
  PR #60.
* **Device tree: WCD9375 playback link** and a MultiMedia3 frontend, so the earpiece plays
  through the WCD EAR PA.
* **Headset detection (MBHC):** surya thresholds.
* **TAS2562 speaker amp:** Digital Volume range and MSB-first writes.
* **Quieter logs:** the q6afe/SoundWire debug prints are toned down.
* **Top microphone: its own SoundWire port.** The WCD9375's ADC2 (the top mic) shared master port 2, at
  the same offset, with ADC1 (the bottom mic): the top mic never arrived, and with both on the bottom mic
  carried full-scale noise. `qcom,tx-port-mapping` is now `<2 3 4 5>`, so ADC2 has port 3. The image's
  call profile takes the uplink from the top mic, which picks up far less of the loudspeaker's echo.

Modem calls: audio both ways
----------------------------

* **q6voice.** On a GSM/LTE call the ADSP mixes the audio itself (MVM, CVS and CVP over APR); the host
  only starts the AFE ports and the voice processor. The q6voice drivers come from
  [sdm845-mainline](https://gitlab.com/sdm845-mainline/linux), with the CVD 2.3 set-up SM7150 needs (detected
  from the CVP version), the codec DMA ports, a zeroed command buffer and the start moved from startup to
  prepare. The device tree adds the APR services and a hostless VoiceMMode1 link, PCM 4 (`hw:0,4`), which
  `q6voiced` opens for every call.
* **Routing** is in UCM: a "Voice Call" verb (earpiece or speaker, bottom mic) that callaudiod selects,
  plus `TERT_MI2S_RX Voice Mixer VoiceMMode1` and `VoiceMMode1 Capture Mixer TX_CODEC_DMA_TX_3` at boot.
* **The uplink needs a running mono capture.** The far end hears nothing until a 1-channel capture
  actually runs on the mic's AFE port (TX_CODEC_DMA_TX_3) alongside the call; a 2-channel one, or one only
  prepared, does not do it. The image runs one on MultiMedia3 for the length of every call
  (`surya-call-uplink`, in the Neo tree).
* `q6cvp` module options: `tx_channels`, `rx_channels`, `sample_rate`, and `mono_channel` (1 = FL, the
  default; downstream says FC). The TX topology is the `VoiceMMode1 TX Topology` control. Its kernel
  default, single-mic echo/noise suppression (0x10F71), sends the far end silence without Xiaomi's
  calibration; the image's UCM sets no processing (0x10F70, 69488) at boot.

Touchscreen and display
-----------------------

* **Double tap to wake.** When the panel turns off, the NT36xxx driver puts the controller into
  wakeup-gesture mode instead of suspending it. A double tap is reported as `KEY_WAKEUP` on a
  separate "NT36XXX Wake Gesture" input device. It is controlled by the `wake_gesture` module
  parameter (on by default).
* **Panel keeps the touch controller powered.** On unprepare, `panel-huaxing-nt36672c` leaves
  reset and supply alone (`keep_powered`, on by default), so the touch side of the TDDI stays
  alive.
* **nt36xxx fix: use-after-free.** `_nt36xxx_boot_download_firmware` released the firmware and
  then read its size and first byte. This faulted in the download worker at boot and could hang
  the phone.
* **The display hand-over at boot.** Between the boot logo and the boot splash the LCD flashed or went
  dark several times; each had its own cause:
  * `qcom-wled` registered at its default brightness (2048 of 4095) and flashed the logo bright: on WLED5 it
    now starts at the bootloader's brightness. It also switches off a WLED5 module the bootloader left on,
    and the panel driver switches it on again once the panel runs.
  * Both nt36672c panel drivers (Huaxing and Tianma) take their reset GPIO as it is instead of asserting
    reset at probe, and light the backlight 100 ms after the video starts: lit with the first frame, the LCD
    showed white.
  * `dispcc-sm7150` leaves a running display PLL0 alone: reconfiguring it under the bootloader's scanout
    flashed the LCD white.
  * `msm` fbdev starts with the bootloader's splash picture: after the first modeset a zeroed buffer showed
    black, glowing at the backlight level, until the splash.

Charging: USB-C power delivery
------------------------------

* **The port can sink power.** The connector was declared source-only with no `sink-pdos`, so
  the phone had nothing to request from a PD charger. Each time, the request failed, the charger
  sent a hard reset about 30 ms later and dropped VBUS, and charging kept starting and stopping.
  The port is now dual-role (`try-power-role = "sink"`) with a 5 V / 3 A sink PDO, and PD chargers
  hold a 5 V contract.
* **USB data ports charge too.** On a standard downstream port (a hub, a computer) the charger ran in USB
  500 mA mode and ignored the programmed input limit, so the phone drew 480 mA, less than it uses with the
  screen on, and slowly emptied while it read "Charging". Above 500 mA the driver now sets the ICL override so
  the programmed limit applies; AICL still lowers the current if the port's voltage sags. On a hub: 1.48 A in.
* **Limits:** the device tree keeps the charger input at 1.5 A, and the driver fixes the battery
  charge current at 1.95 A. The input limit (`current_max`) can be raised from userspace, for
  example by a temperature-guarded service. 9 V PD needs driver work. The 33 W fast charging
  uses an external charge pump with no mainline driver.

Suspend
-------

Suspend to idle works with this kernel as it is; the image's sleep daemon (`surya-sleepd`) puts the phone
to sleep when the screen goes off. The one surya-specific sleep change in this tree is the charger's (below).
What a sleep needs, and what is still open:

* **The FastRPC servers have to be stopped before a sleep** (both hexagonrpcd instances, not only the
  sensors' one). Frozen through a sleep, they crash the ADSP at resume, and after its restart the VA macro
  and the SoundWire controller stay in a runtime PM error: no sound until a reboot.
* **The Bluetooth UART's RX wake ends every sleep after about a second** (its wake IRQ fires as soon as it
  is armed). With `power/wakeup` of `88c000.serial` set to `disabled` the phone sleeps until something
  real wakes it. The kernel fix is open.
* **Fixed: on a computer's USB port the phone never slept.** AICL reruns every few seconds, and on a port
  that cannot supply the programmed limit it walks the limit up and down at every rerun: about three
  input-limit changes a second. `qcom_smbx` reported each one with `power_supply_changed()`, which holds the
  supply's wakeup source, and a wakeup event aborts a sleep in progress. It now reports a change only when
  the limit moved by 200 mA or more, or input current started or stopped.
* Wi-Fi disconnects at every suspend and is back about 4 s after the resume.

Vibration: AW8624 haptics
-------------------------

* **The phone vibrates.** Its haptics chip is an Awinic AW8624, which the `aw8695-haptics` driver
  accepted by chip ID only. The driver's setup is for the AW8695: boost, analog trim and f0
  calibration. On the AW8624 those addresses are other registers (there is no boost), and writing
  them held the chip in under-voltage lockout. It fell back to standby the moment it was made
  active, so every effect ran with GO set and the motor never moved. For the AW8624 the driver now
  stops after the interrupt and PWM setup, leaves the boost bits alone, polls `GLB_STATE` at its
  own address (0x47), and plays a sine at the motor's resonance (24 kHz / 117 = 205 Hz).
* **Strength.** The rumble magnitude sets the AW8624's RAM playback gain (`DATDBG`, 0x3b), so
  softer effects play softer, and feedbackd's `max-haptic-strength` setting controls how strongly
  the phone vibrates.
* **Short effects run their full length.** Starting an effect is some 8 ms of I2C before the GO bit, and
  ff-memless times the stop from the request, so a 5 ms effect drove the motor for under 1 ms and a 12 ms
  one for 4 ms (measured), too short to feel. The stop is now timed from when GO started the motor: it runs
  for as long as the effect asked, and the AW8624 for at least 15 ms (three cycles at 205 Hz), what a key
  tick needs. 15 ms may still be tuned by feel.
* Pulses closer than about 150 ms apart blur together: the motor keeps ringing for that long after
  each pulse.

Status LED and GPS
------------------

* **Status LED works.** The white LED above the screen is driven by the PM6150L LPG as
  `white:status` (0-511), with the kernel's pattern trigger. The pattern trigger ramps between
  points, so it fades in and out on its own (`echo "0 1200 511 1200 0 2600 0 0" > pattern` breathes).
  feedbackd's udev rule puts it on that trigger and gives its group the pattern file.
* **GPS works.** The modem's GNSS receiver (GPS, GLONASS and others, behind QMI LOC) reports
  satellites through ModemManager and geoclue. The kernel side was already in place (the PD mapper
  registers the modem's `gps_service`). What blocked it was in the modem: it comes up with its GNSS
  engine locked, and every start fails with QMI LOC `GeneralError` (46). Clearing the lock once per boot
  is enough: `qmicli -d qrtr://0 -p --loc-set-engine-lock=none`.

Wi-Fi: 5 GHz channels
---------------------

* **`ath.regdomain=<country>`**, a new module option. The phone's Wi-Fi calibration names China
  (CN). On top of that, for any country in the calibration, the ath driver first applied its most
  restrictive world table, which leaves out 5 GHz channels 100-144 permanently. Networks on those
  channels were invisible. The option replaces the calibration's country with the one the phone is
  used in, and starts from a table covering all of 2.4 GHz and 5 GHz up to 5875 MHz, so that
  country's rules decide. Set it with `options ath regdomain=DE` in `/etc/modprobe.d/`.

Video: Venus (IRIS1) decode and encode
--------------------------------------

The SM7150's video core is an IRIS1 with one pipe, a generation mainline's venus driver knows only by name.

* **The driver** (`drivers/media/platform/qcom/venus`) is taken whole from the SM8150 IRIS1 port of
  [Aospa-raphael-unofficial/linux](https://github.com/Aospa-raphael-unofficial/linux) (branch `venus`, about
  40 commits by snowf14k3), plus `sm7150_res` (one pipe, the SM7150's video clocks in the port's roles, the
  `cx` performance domain) and the CVP domain the SM7150's firmware boots in (CTRL_INIT bit 1 and a second,
  1 MiB queue table).
* **The encoder's persistent buffer must sit in the secure non-pixel domain** (SID 0x1084, VMID
  CP_NON_PIXEL), even for normal sessions, and the SMMU's page tables for that domain must be shared with
  that VM (`iommu/arm-smmu: share Qualcomm secure-domain page tables`, from the same port; DT
  `qcom,secure-vmid`). With the buffer alone the bus stalled on the first frame. The secure device's
  addresses are limited in the driver: done with `dma-ranges`, as in the SM8150 port, it left the phone
  with no ZONE_DMA, and the touchscreen could not load its firmware.
* **192 MHz.** This phone's video multipipe is fused off (QFPROM), which rates it for 200 MHz only;
  `videocc-sm7150` runs the video clock at 192 MHz as downstream does, and the DT has that one rate.
* **Seeking.** GStreamer's decoder flushes for a seek with STREAMOFF on OUTPUT, STREAMOFF and STREAMON on
  CAPTURE, then STREAMON on OUTPUT. Venus then never decoded again: the capture buffers queued before the
  restart stayed in the m2m queue, and the decoded picture buffers were not handed back. `vdec` now gives
  both to the firmware when CAPTURE comes back during a seek.
* H.264, HEVC, VP8 and VP9 decode, H.264 and HEVC encode; GStreamer's
  `v4l2h264enc` records the camera at 1080p 30 fps. The video core powers down when idle.

Cameras: CAMSS and sensors
--------------------------

Status of the cameras:

| Camera     | Sensor                    | PHY                                         | State   |
|------------|---------------------------|---------------------------------------------|---------|
| Main       | Sony IMX682 + DW9800 VCM  | CSIPHY0, C-PHY                              | working |
| Front      | Samsung S5K3T2            | CSIPHY1, D-PHY, behind the CAM_SEL mux      | working |
| Macro      | SK Hynix Hi-259           | CSIPHY2, combo mode (data lane 2, clock 3)  | working |
| Ultrawide  | SK Hynix Hi-1337          | CSIPHY1, behind the CAM_SEL mux             | working (4208x3120, and 2096x1560 at 60 fps) |
| Depth      | OmniVision OV02B1B        |                                             | no driver |

### CAMSS: every SM7150 CSIPHY needs `csiphy0_clk`

* **The key fix.** The vendor device tree lists `CAM_CC_CSIPHY0_CLK` for all four CSIPHYs.
  Mainline only enabled it for CSIPHY0. Without it, CSIPHY1-3 saw the lanes toggle but never
  passed a packet to the CSID: zero SoT, while the CSID test pattern generator still worked.
  This is what blocked the front, macro and ultrawide cameras.
* **refgen** is voted as a CSIPHY supply.
* **CSIPHY RX clock** starts at the vendor's 384 MHz.
* **CSIPHY rails** run at the vendor voltages (vdda-phy 0.88 V, vdda-pll 1.2 V).

### CAMSS: SM7150 CSIPHY tables, combo mode and C-PHY

* **Lane tables.** Qualcomm's v1.2 lane tables (`csiphy_2ph_v1_2_reg` and the combo-mode variant)
  are programmed lane by lane, as the vendor driver does.
* **Combo mode** lets two sensors share one PHY. `clock-lanes = <3>` selects it, and the second
  sensor is clocked on lane 3.
* **C-PHY** for the main camera: the bus type is carried from the endpoint to the CSIPHY and CSID,
  with Qualcomm's `csiphy_3ph_v1_2_reg` trio tables.
* **Bus clocks.** VFE1 and VFE_LITE get the cpas/camnoc bus clocks.
* **Buffer flush fix.** `vfe_flush_buffers()` now clears every returned buffer. Before, a
  stop/start left requests queued, libcamera aborted in `stop()`, and switching cameras killed
  WirePlumber.
* **Shared PHYs.** Sensors that share a CSIPHY get mutable links (only the first is enabled), so
  libcamera can switch between them.
* **Debug cleanup.** The bring-up debug module parameters and trace prints are removed, and so is a stray
  `#define DEBUG` in `v4l2-subdev` that logged every link validation of each camera start.
* **A failed stream start no longer breaks every camera.** When one stage failed to start, the
  stages already started stayed marked as streaming, and since all four cameras share CSID0/VFE0,
  every later start was silently skipped until a reboot. The started stages are now stopped again.
* **The main camera's lens driver (DW9807) stays quiet while the sensor is off.** It used to send
  writes into a powered-down lens, and the bus timeouts broke the IMX682's own start. Waking the
  lens now only switches on its supply. It is first addressed on a focus write, while streaming.
* **The IMX682 retries its init writes** (up to three times) when the shared bus hiccups. Switching
  between all four cameras in GNOME Snapshot works reliably (30/30 in a stress test).
* **Bright enough indoors with libcamera's software ISP.** Its auto exposure drives only the
  analogue gain, within the 30 fps frame, and at their maximum every camera stayed far too dark in
  an evening room. The sensors get a fixed digital gain on all four colour filter positions (SMIA
  `0x020e`-`0x0214` on the IMX682 and S5K3T2, `0x0214`-`0x021a` on the Hi-1337): 4x main, 4x
  front, about 16x ultrawide. The auto exposure lowers the analogue gain and the exposure itself in
  bright light. The Hi-259 macro's analogue gain goes up to 14x (MediaTek's driver stops at 8x;
  the gain code allows about 15x). Setting only `0x020e`, the green-next-to-red position, left the
  front camera a green and magenta grid.
* **60 fps video modes**, from the vendor sensor module data:
  * IMX682 (main): 3840x2160 (2x2 binned, 16:9 crop) and 1920x1080 (4x4 binned) at 60 fps, beside the full
    4624x3472. The mode is picked by the nearest size; pixel rate, link frequency and blanking follow it,
    vblank is reset on a mode change, and hblank is advertised exactly, so libcamera gets the line length
    right. libcamera's software ISP streams 1080p at 59 fps and 720p at 121 fps.
  * S5K3T2 (front): 2592x1940 (2x2 binned) at 60 fps and 1280x720.
  * Hi-1337 (ultrawide): the 2x2 binned mode at 60 fps, cropped to 2096 wide (below).
* **DW9807 Smart Actuator Control** (`sac_mode`, `sac_timing`): the driver never programmed the lens
  driver's mode (0x06) and resonance timing (0x07), so it ran in direct mode. The two module parameters set
  them, with ringing control; the values belong to the lens module (this phone's IMX682 module: 0x40 and
  0x7a, from the vendor's actuator settings). Unset, nothing changes.
* **Hi-1337 Bayer order is GBRG.** Declared GRBG, as on the Hi-847, red and blue came out swapped:
  a yellow floor and red objects turned blue, and the software ISP's white balance flickered
  between teal and neutral.

### New drivers

* **`regulator: wl2866d`:** Will Semiconductor WL2866D camera LDO (2x DVDD, 2x AVDD over I2C).
  There is no public datasheet. The register layout comes from Xiaomi's driver, and the voltage
  encoding is fitted to the vendor codes.
* **`media: i2c: hi259`:** SK Hynix Hi-259, the 2 MP macro camera. 1600x1200 SBGGR10 on one
  lane at 30 fps, with exposure, gain and test pattern. The tables are decoded from the vendor
  CamX module.
* **`media: i2c: hi1337`:** SK Hynix Hi-1337, the 13 MP ultrawide, at 4208x3120 and 2096x1560 (binned).
  The register tables are written with `cci_multi_reg_write()`. `regmap_multi_reg_write()` on
  the 8-bit CCI regmap dropped the 16-bit width, so only the low byte of each entry went out.
* **Front and ultrawide no longer knock each other out at boot.** Both sensors share CSIPHY1 and
  power up at probe to read their chip id. The one that probed second found the shared switch busy,
  failed, and took every camera with it, because CAMSS waits for all sensors. A probe now waits up
  to 3 s for the switch.
* **Hi-1337's binned mode is 2096 wide.** At 2104 pixels a RAW10 line is 2630 bytes, not a multiple of 4,
  and the frames arrived with bands of shifted colour and garbled lines. Cropped to 2096 the binned mode
  works, at 60 fps (frame length 1666).
* **`media: i2c: imx682`, `media: i2c: s5k3t2`:** the main and front sensors, from
  [woodyst/surya-pmos](https://github.com/woodyst/surya-pmos).
* **`media: dw9807-vcm`:** the main camera's DW9800 actuator gets its vcc (coil) supply. I2C errors
  on resume are non-fatal, and the regulator is no longer disabled twice.

### CAM_SEL mux

The front and ultrawide sensors share CSIPHY1 through a board switch (pm6150 GPIO). It is now a
`gpio-mux` controller: s5k3t2 selects state 0 and hi1337 selects state 1. The unused
`camera_mipi_switch_en` regulator is dropped so the mux GPIOs stay free.

### Device tree

* **Camera power:** the WL2866D LDOs.
* **Sensor nodes:** all four sensors and the actuator, each on its CSIPHY.
* **Rotation:** 270 for the cameras.

Known issues
------------

* **IMX682 probe at boot:** a bus timeout during its chip id read used to fail the probe, and with it
  every camera (CAMSS waits for all sensors). The probe now power-cycles the sensor and retries.
* **Untested:** headset microphone and headphone output.
* **Camera image quality:** no libcamera tuning yet.
* **Sleep:** see Suspend.

Booting: U-Boot
---------------

The phone boots Xiaomi's ABL → U-Boot → systemd-boot → this kernel. A ready-to-flash U-Boot, its build inputs
and the steps are in [`u-boot/`](u-boot/README.md): `fastboot flash boot u-boot/u-boot-surya.img`. It boots
quietly over the POCO logo and expects the system as a disk image (ESP + root) in the `userdata` partition.
It works on both panel variants: it reads the panel ABL reports and loads the matching device tree from the ESP
(`/dtb/qcom/sm7150-xiaomi-surya-huaxing.dtb` or `-tianma.dtb`), so one system image boots either phone. How to find out which panel a phone has (from recovery, Android or
Linux) is in the [U-Boot README](u-boot/README.md#which-panel-does-my-phone-have).

Building
--------

Cross-compile for arm64 with the SM7150 config fragment and the drivers above:

    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=out defconfig sm7150.config
    ./scripts/config --file out/.config \
        -m REGULATOR_WL2866D -m VIDEO_QCOM_CAMSS -m VIDEO_HI259 -m VIDEO_HI1337 \
        -m VIDEO_IMX682 -m VIDEO_S5K3T2 -m VIDEO_DW9807_VCM \
        -m SND_SOC_WCD937X -m SND_SOC_WCD937X_SDW -m SND_SOC_TAS2562 \
        -e TOUCHSCREEN_NT36XXX_SPI \
        -e ANDROID_BINDER_IPC --set-str ANDROID_BINDER_DEVICES binder,hwbinder,vndbinder -e PSI
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=out olddefconfig
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=out -j$(nproc) vmlinuz.efi dtbs modules

The binder devices and PSI are for Waydroid (Android apps as windows): it uses the legacy `/dev/binder`,
`/dev/hwbinder` and `/dev/vndbinder` (no binderfs), and Android's low-memory killer runs on PSI.

There are two device trees, one per display panel. Pick the one that matches your phone:

* `arch/arm64/boot/dts/qcom/sm7150-xiaomi-surya-huaxing.dtb`
* `arch/arm64/boot/dts/qcom/sm7150-xiaomi-surya-tianma.dtb`

Credits
-------

* **[sm7150-mainline](https://github.com/sm7150-mainline/linux):** the SM7150 platform and surya
  bring-up (Danila Tikhonov, Jens/Aelin Reidel, David Wronek, Luca Weiss and others). WCD9375
  audio for davinci by Artem Hutsaliuk (PR #60).
* **[sdm845-mainline](https://gitlab.com/sdm845-mainline/linux):** the q6voice drivers (Stephan Gerhold,
  Vincent Knecht, Joel Selvaraj, Otto Pflüger) and their CVD 2.3 sequence (Richard Acayan).
* **[woodyst/surya-pmos](https://github.com/woodyst/surya-pmos):** IMX682 / S5K3T2 drivers,
  C-PHY, the DW9807 fixes, refgen/rails/384 MHz.
* **[asidko/qcom-camss-mainline](https://github.com/asidko/qcom-camss-mainline):** the VFE
  buffer-flush fix.
* **[Aospa-raphael-unofficial/linux](https://github.com/Aospa-raphael-unofficial/linux):** the IRIS1
  venus driver and the shared secure-domain SMMU page tables (snowf14k3).

The original kernel README is in [`README`](README).

