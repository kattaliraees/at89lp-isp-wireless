# AT89LP ISP Programming Reference

This document serves as a quick-reference guide for implementing the ISP programmer for the Microchip/Atmel **AT89LP6440** (and general AT89LP series) microcontrollers, based on Atmel application note **doc3593.pdf**.

---

## 1. SPI Interface Configuration

* **Clock Mode**: SPI Mode 0 (CPOL = 0, CPHA = 0)
  * Data is sampled on the rising edge of SCK.
  * Data is changed/output on the falling edge of SCK.
* **Bit Order**: MSB (Most Significant Bit) First.
* **Maximum SCK Frequency**: 1 MHz (minimum SCK cycle time $t_{SCK} = 1\ \mu\text{s}$).
* **Hardware Pins**:
  * **SCK** (Target clock input, driven by programmer master)
  * **MOSI** (Target data input, driven by programmer master)
  * **MISO** (Target data output, driven by target slave)
  * **$\overline{\text{SS}}$** (Slave Select - active low, toggled per command frame)
  * **$\overline{\text{RST}}$** (Target Reset - held low/active for the entire programming session)

---

## 2. Timing Requirements

| Symbol | Parameter | Min Value | Comment |
| :--- | :--- | :--- | :--- |
| $t_{STL}$ | Reset ($\overline{\text{RST}}$) low settling time | 100 ns | Delay after pulling reset low before starting SPI communication |
| $t_{ZSS}$ | SCK setup to SS low | 25 ns | SCK must be driven low at least 25ns before pulling SS low |
| $t_{SSE}$ | SS Enable lead time | $t_{SLSH}$ (Clock Low) | Delay after pulling SS low before first SCK rising edge |
| $t_{SSD}$ | SS Disable lag time | $t_{SLSH}$ (Clock Low) | Delay after last SCK falling edge before pulling SS high |
| $t_{SSZ}$ | SCK hold after SS high | 25 ns | SCK must remain low at least 25ns after pulling SS high |

---

## 3. ISP Programming Commands

Every command frame is bounded by driving $\overline{\text{SS}}$ low at the beginning, and high at the end. Every command packet must start with the two preamble bytes `0xAA` and `0x55`.

| Command | Preamble | Opcode | Address High | Address Low | Data Bytes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Programming Enable** | `0xAA`, `0x55` | `0xAC` | `0x53` | `0x00` (dummy) | Read `0x53` on MISO during dummy byte |
| **Chip Erase** | `0xAA`, `0x55` | `0x8A` | — | — | — |
| **Read Status** | `0xAA`, `0x55` | `0x60` | `0x00` (dummy) | `0x00` (dummy) | Read Status Out byte |
| **Load Page Buffer** | `0xAA`, `0x55` | `0x51` | `Addr High` | `Addr Low` | Write `Data In 0 ... Data In n` (1 to 64 bytes) |
| **Write Code Page** | `0xAA`, `0x55` | `0x50` | `Addr High` | `Addr Low` | Write `Data In 0 ... Data In n` (1 to 64 bytes) |
| **Write Code Page (Auto-Erase)** | `0xAA`, `0x55` | `0x70` | `Addr High` | `Addr Low` | Write `Data In 0 ... Data In n` (1 to 64 bytes) |
| **Read Code Page** | `0xAA`, `0x55` | `0x30` | `Addr High` | `Addr Low` | Read `Data Out 0 ... Data Out n` (1 to 64 bytes) |
| **Read Atmel Signature Row** | `0xAA`, `0x55` | `0x38` | `Addr High` | `Addr Low` | Read `Data Out 0 ... Data Out n` (3 bytes for IDs) |
| **Read User Fuses** | `0xAA`, `0x55` | `0x61` | `Addr High` | `Addr Low` | Read `Data Out 0 ... Data Out n` |
| **Write User Fuses** | `0xAA`, `0x55` | `0xE1` | `Addr High` | `Addr Low` | Write `Data In 0 ... Data In n` |
| **Write User Fuses (Auto-Erase)** | `0xAA`, `0x55` | `0xF1` | `Addr High` | `Addr Low` | Write `Data In 0 ... Data In n` |
| **Read Lock Bits** | `0xAA`, `0x55` | `0x64` | `Addr High` | `Addr Low` | Read `Data Out 0 ... Data Out n` |
| **Write Lock Bits** | `0xAA`, `0x55` | `0xE4` | `Addr High` | `Addr Low` | Write `Data In 0 ... Data In n` |

---

## 4. Status Register (Read Only)

The Status Register is accessed via the **Read Status** command. It is mainly used to poll the $\overline{\text{BUSY}}$ bit to monitor erase/programming completion.

| Bit | Name | Function | Description |
| :---: | :--- | :--- | :--- |
| **7** | — | Reserved | Read as undefined. |
| **6** | — | Reserved | Read as undefined. |
| **5** | — | Reserved | Read as undefined. |
| **4** | — | Reserved | Read as undefined. |
| **3** | $\overline{\text{LOAD}}$ | Load Flag | Cleared low by "Load Page Buffer" command. Set high by the next memory write. |
| **2** | `SUCCESS` | Success Flag | Cleared low at start of programming cycle. Set high if cycle completes without brownout. |
| **1** | $\overline{\text{WRTINH}}$ | Write Inhibit | Cleared low by brownout detector if VCC drops too low. Forces $\overline{\text{BUSY}}$ low. |
| **0** | $\overline{\text{BUSY}}$ | Busy Flag | **Cleared low (0)** when target is busy programming.<br>**Set high (1)** when target is idle/done. |

---

## 5. Programming Sequences

### A. ISP Entry Sequence (Operational/CPU execution mode)
1. Drive SCK low.
2. Drive $\overline{\text{RST}}$ active (low).
3. Drive $\overline{\text{SS}}$ high.
4. Delay at least $t_{STL}$ (100 ns).
5. Drive $\overline{\text{SS}}$ low.
6. Transmit sequence: `0xAA`, `0x55`, `0xAC`, `0x53`.
7. Transmit dummy byte `0x00` and read incoming byte on MISO.
8. If returned byte is **`0x53`**, entry is successful.
9. Drive $\overline{\text{SS}}$ high.

### B. Polling Busy Status
1. Drive $\overline{\text{SS}}$ low.
2. Transmit sequence: `0xAA`, `0x55`, `0x60`, `0x00`, `0x00`.
3. Transmit dummy byte `0x00` and read incoming byte.
4. Drive $\overline{\text{SS}}$ high.
5. Check if **Bit 0** of the returned byte is `1` (Ready). If `0`, repeat after a small delay.

### C. Reading Device Signature (3 Bytes)
1. Drive $\overline{\text{SS}}$ low.
2. Transmit sequence: `0xAA`, `0x55`, `0x38`, `0x00`, `0x00` (Address high/low starts at `0x0000`).
3. Transmit 3 dummy bytes (`0x00`) to read signature bytes 0, 1, and 2.
4. Drive $\overline{\text{SS}}$ high.
5. Signature bytes map to:
   * **Byte 0**: Manufacturer ID (`0x1E` for Microchip/Atmel)
   * **Byte 1**: Family ID
   * **Byte 2**: Device ID (specific to AT89LP6440)

### D. Writing Page (Page Buffer method)
1. **Load Buffer**:
   * Drive $\overline{\text{SS}}$ low.
   * Transmit sequence: `0xAA`, `0x55`, `0x51`, `Page Addr High`, `Page Addr Low`.
   * Transmit up to 64 bytes of page data.
   * Drive $\overline{\text{SS}}$ high.
2. **Commit Page**:
   * Drive $\overline{\text{SS}}$ low.
   * Transmit sequence: `0xAA`, `0x55`, `0x70`, `Page Addr High`, `Page Addr Low`.
   * (Optionally omit loading data bytes here if the buffer is already full).
   * Drive $\overline{\text{SS}}$ high.
3. **Wait**: Poll status until $\overline{\text{BUSY}}$ returns `1`.

### E. ISP Exit Sequence
1. Drive SCK low.
2. Wait $t_{SSD}$ (approx. 1 $\mu\text{s}$).
3. Bring $\overline{\text{SS}}$ high.
4. Tristate MOSI.
5. Wait $t_{SSZ}$ (25 ns) and bring $\overline{\text{RST}}$ high.
6. Tristate SCK and $\overline{\text{SS}}$.
