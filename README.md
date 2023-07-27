
# SmartMatrix IDF

[![GPLv3 License](https://img.shields.io/badge/License-GPL%20v3-yellow.svg)](https://opensource.org/licenses/)
[![CodeFactor](https://www.codefactor.io/repository/github/acvigue/smartmatrix-idf/badge)](https://www.codefactor.io/repository/github/acvigue/smartmatrix-idf)

An infinitely customizable internet-connected smart RGB LED matrix powered by an ESP32

Currently only supports the ESP32-S3.

## Configuration

Copy the `secrets.h.example` file to `secrets.h` and fill in relevant information. OTA can be disabled by leaving the manifest URL empty

To use a Tidbyt, uncomment the #define PINDEF_TIDBYT line

To deploy, compile and upload using ESP-IDF v5.2

## Installation

Use VSCode with the ESP-IDF extension to build & flash this project to your device. 

## Requirements

This project requires a MQTT server and the [SmartMatrixServer](https://github.com/acvigue/SmartMatrixServer) to function properly.

## Authors

- [@acvigue](https://www.github.com/acvigue)

