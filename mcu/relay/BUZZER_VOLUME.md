# Buzzer Volume

The Waveshare ESP32-S3-Relay-6CH passive buzzer is driven on GPIO21.

v21 reduces the buzzer drive from the prior approximately 50% duty bit-bang chirp to 5% duty:

```cpp
static const uint8_t BUZZER_DUTY_PERCENT = 5;
```

This is roughly 1/10 the prior high-time drive. It preserves the existing chirp/tone behavior but makes it quieter.

To adjust later:

```cpp
0   = silent
5   = current v21 quiet setting
10  = louder
50  = old approximate behavior
```
