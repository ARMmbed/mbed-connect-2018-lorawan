/**
 * Copyright (c) 2017, Arm Limited and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mbed.h"
#include "mbed_events.h"
#include "mbed_trace.h"
#include "LoRaWANInterface.h"
#include "CayenneLPP.h"

#ifdef TARGET_SIMULATOR
#include "Sht31.h"
#include "SX1276_LoRaRadio.h"
SX1276_LoRaRadio radio(D11, D12, D13, D10, A0, D2, D3, D4, D5, D8, D9, NC, NC, NC, NC, A4, NC, NC);
#else
#include "lora_radio_helper.h"
#endif

#define TX_INTERVAL         10000

static uint8_t LORAWAN_DEV_EUI[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static uint8_t LORAWAN_APP_EUI[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static uint8_t LORAWAN_APP_KEY[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

static EventQueue ev_queue;
static void lora_event_handler(lorawan_event_t event);
static LoRaWANInterface lorawan(radio);
static lorawan_app_callbacks_t callbacks;

int main (void)
{
    mbed_trace_init();

    lorawan_status_t retcode;

    // Initialize LoRaWAN stack
    if (lorawan.initialize(&ev_queue) != LORAWAN_STATUS_OK) {
        printf("LoRa initialization failed! \r\n");
        return -1;
    }

    printf("Mbed LoRaWANStack initialized \r\n");

    // prepare application callbacks
    callbacks.events = mbed::callback(lora_event_handler);
    lorawan.add_app_callbacks(&callbacks);

    // Set number of retries in case of CONFIRMED messages
    if (lorawan.set_confirmed_msg_retries(3) != LORAWAN_STATUS_OK) {
        printf("set_confirmed_msg_retries failed! \r\n\r\n");
        return -1;
    }

    // Enable adaptive data rate
    if (lorawan.enable_adaptive_datarate() != LORAWAN_STATUS_OK) {
        printf("enable_adaptive_datarate failed! \r\n");
        return -1;
    }

    printf("Adaptive data  rate (ADR) - Enabled \r\n");

    lorawan_connect_t connect_params;
    connect_params.connect_type = LORAWAN_CONNECTION_OTAA;

    connect_params.connection_u.otaa.dev_eui = LORAWAN_DEV_EUI;
    connect_params.connection_u.otaa.app_eui = LORAWAN_APP_EUI;
    connect_params.connection_u.otaa.app_key = LORAWAN_APP_KEY;
    connect_params.connection_u.otaa.nb_trials = 10;

    retcode = lorawan.connect(connect_params);

    if (retcode == LORAWAN_STATUS_OK || retcode == LORAWAN_STATUS_CONNECT_IN_PROGRESS) {
        printf("Connection - In Progress ...\r\n");
    } else {
        printf("Connection error, code = %d \r\n", retcode);
        return -1;
    }

    // make your event queue dispatching events forever
    ev_queue.dispatch_forever();
}

/**
 * Sends a message to the Network Server
 */
static void send_message() {

#ifdef TARGET_SIMULATOR
    static Sht31 sht31(I2C_SDA, I2C_SCL);
    float temperature = sht31.readTemperature();
#else
    float temperature = static_cast<float>(rand()) / static_cast<float>(RAND_MAX / 50) + 10.0f;
#endif

    CayenneLPP payload(50);
    payload.addTemperature(1, temperature);

    int16_t retcode = lorawan.send(15, payload.getBuffer(), payload.getSize(), MSG_UNCONFIRMED_FLAG);

    if (retcode < 0) {
        retcode == LORAWAN_STATUS_WOULD_BLOCK ? printf("send - Duty cycle violation\r\n")
                : printf("send() - Error code %d \r\n", retcode);

        if (retcode == LORAWAN_STATUS_WOULD_BLOCK) {
            //retry in 3 seconds
            ev_queue.call_in(3000, send_message);
        }
        else {
            ev_queue.call_in(TX_INTERVAL, send_message);
        }
        return;
    }

    ev_queue.call_in(TX_INTERVAL, send_message);

    printf("%d bytes scheduled for transmission \r\n", retcode);
}

/**
 * Receive a message from the Network Server
 */
static void receive_message()
{
    uint8_t rx_buffer[50] = { 0 };
    int16_t retcode = lorawan.receive(15, rx_buffer,
                                      sizeof(rx_buffer),
                                      MSG_CONFIRMED_FLAG|MSG_UNCONFIRMED_FLAG);

    if (retcode < 0) {
        printf("receive() - Error code %d \r\n", retcode);
        return;
    }

    printf("RX Data (%d bytes): ", retcode);
    for (uint8_t i = 0; i < retcode; i++) {
        printf("%02x ", rx_buffer[i]);
    }
    printf("\r\n");
}

/**
 * Event handler
 */
static void lora_event_handler(lorawan_event_t event)
{
    switch (event) {
        case CONNECTED:
            printf("Connection - Successful \r\n");
            ev_queue.call_in(TX_INTERVAL, send_message);
            break;
        case DISCONNECTED:
            ev_queue.break_dispatch();
            printf("Disconnected Successfully \r\n");
            break;
        case TX_DONE:
            printf("Message Sent to Network Server \r\n");
            break;
        case TX_TIMEOUT:
        case TX_ERROR:
        case TX_CRYPTO_ERROR:
        case TX_SCHEDULING_ERROR:
            printf("Transmission Error - EventCode = %d \r\n", event);
            break;
        case RX_DONE:
            printf("Received message from Network Server \r\n");
            receive_message();
            break;
        case RX_TIMEOUT:
        case RX_ERROR:
            printf("Error in reception - Code = %d \r\n", event);
            break;
        case JOIN_FAILURE:
            printf("OTAA Failed - Check Keys \r\n");
            break;
        case UPLINK_REQUIRED:
            printf("Uplink required by NS \r\n");
            send_message();
            break;
        default:
            MBED_ASSERT("Unknown Event");
    }
}
