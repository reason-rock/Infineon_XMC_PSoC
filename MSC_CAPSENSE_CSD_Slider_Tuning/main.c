/******************************************************************************
* File Name: main.c
*
* Description: This is the source code for the PSoC 4 MSC Self-Capacitance
* Slider Tuning code example for ModusToolbox.
*
* Related Document: See README.md 
*
*******************************************************************************
* Copyright 2021-2023, Cypress Semiconductor Corporation (an Infineon company) or
* an affiliate of Cypress Semiconductor Corporation.  All rights reserved.
*
* This software, including source code, documentation and related
* materials ("Software") is owned by Cypress Semiconductor Corporation
* or one of its affiliates ("Cypress") and is protected by and subject to
* worldwide patent protection (United States and foreign),
* United States copyright laws and international treaty provisions.
* Therefore, you may use this Software only as provided in the license
* agreement accompanying the software package from which you
* obtained this Software ("EULA").
* If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
* non-transferable license to copy, modify, and compile the Software
* source code solely for use in connection with Cypress's
* integrated circuit products.  Any reproduction, modification, translation,
* compilation, or representation of this Software except as specified
* above is prohibited without the express written permission of Cypress.
*
* Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. Cypress
* reserves the right to make changes to the Software without notice. Cypress
* does not assume any liability arising out of the application or use of the
* Software or any product or circuit described in the Software. Cypress does
* not authorize its products for use in any products where a malfunction or
* failure of the Cypress product may reasonably be expected to result in
* significant property damage, injury or death ("High Risk Product"). By
* including Cypress's product in a High Risk Product, the manufacturer
* of such system or application assumes all risk of such use and in doing
* so agrees to indemnify Cypress against all liability.
*******************************************************************************/

/*******************************************************************************
 * Include header files
 ******************************************************************************/
#include "cy_pdl.h"
#include "cybsp.h"
#include "cycfg.h"
#include "cycfg_capsense.h"

#include <stdio.h> // For terminal output


/*******************************************************************************
* Macros
*******************************************************************************/
#define CAPSENSE_MSC0_INTR_PRIORITY      (3u)
#define CY_ASSERT_FAILED                 (0u)
#define MSC_CAPSENSE_WIDGET_INACTIVE     (0u)

#if CY_CAPSENSE_BIST_EN
#define NUMBER_OF_SLIDER_SEGMENTS        (8u)
#define SHIELD_MEAS_SKIP_CH_MASK         (0u)
#endif

/* EZI2C interrupt priority must be higher than CapSense interrupt. */
#define EZI2C_INTR_PRIORITY              (2u)

#define LED_ON                      (0u) // LED ON OFF
#define LED_OFF                     (1u)
/*******************************************************************************
* Global Variables
*******************************************************************************/
cy_stc_scb_ezi2c_context_t ezi2c_context;

/* Variables for Sensor Cp measurement */
#if CY_CAPSENSE_BIST_EN
uint32_t sensor_id;
uint32_t sense_cap[NUMBER_OF_SLIDER_SEGMENTS];
uint32_t shield_cap;
cy_en_capsense_bist_status_t sensor_meas_status[NUMBER_OF_SLIDER_SEGMENTS];
cy_en_capsense_bist_status_t shield_meas_status;
#endif

/*******************************************************************************
* Function Prototypes
*******************************************************************************/
static void initialize_capsense(void);
static void capsense_msc0_isr(void);
static void ezi2c_isr(void);
static void initialize_capsense_tuner(void);
// static void led_control(void);

#if CY_CAPSENSE_BIST_EN
static void measure_cp(void);
#endif


static void process_slider_position(void); // slider position
static void initialize_uart(void); // UART init
/*******************************************************************************
* Function Name: main
********************************************************************************
* Summary:
*  System entrance point. This function performs
*  - initial setup of device
*  - initialize CapSense
*  - initialize tuner communication
*  - scan touch input continuously
*
* Return:
*  int
*
*******************************************************************************/
int main(void)
{
    cy_rslt_t result = CY_RSLT_SUCCESS;

    /* Initialize the device and board peripherals */
    result = cybsp_init();

    /* Board init failed. Stop program execution */
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(CY_ASSERT_FAILED);
    }

    /* Enable global interrupts */
    __enable_irq();

    /* Initialize EZI2C */
    initialize_capsense_tuner();

    /* Initialize MSC CapSense */
    initialize_capsense();

    /* Initialize UART */
    initialize_uart();


    #if CY_CAPSENSE_BIST_EN
    measure_cp(); /* Measure the sensor capacitance using BIST */
    #endif

    /* Start the first scan */
    Cy_CapSense_ScanAllSlots(&cy_capsense_context);

    for (;;)
    {
        if(CY_CAPSENSE_NOT_BUSY == Cy_CapSense_IsBusy(&cy_capsense_context))
        {
            /* Process all widgets */
            Cy_CapSense_ProcessAllWidgets(&cy_capsense_context);

            /* Turns LED ON/OFF based on Slider status */
            // led_control();
            process_slider_position();

            /* Establishes synchronized communication with the CapSense Tuner tool */
            Cy_CapSense_RunTuner(&cy_capsense_context);

            /* Start the next scan */
            Cy_CapSense_ScanAllWidgets(&cy_capsense_context);
        }
    }
}


/*******************************************************************************
* Function Name: initialize_capsense
********************************************************************************
* Summary:
*  This function initializes the CapSense blocks and configures the CapSense
*  interrupt.
*
*******************************************************************************/
static void initialize_capsense(void)
{
    cy_capsense_status_t status = CY_CAPSENSE_STATUS_SUCCESS;

    /* CapSense interrupt configuration MSC 0 */
    const cy_stc_sysint_t capsense_msc0_interrupt_config =
    {
        .intrSrc = CY_MSC0_IRQ,
        .intrPriority = CAPSENSE_MSC0_INTR_PRIORITY,
    };

    /* Capture the MSC HW block and initialize it to the default state. */
    status = Cy_CapSense_Init(&cy_capsense_context);

    if (status != CY_CAPSENSE_STATUS_SUCCESS)
    {
        /* CapSense initialization failed, the middleware may not operate
         * as expected, and repeating of initialization is required.*/
        CY_ASSERT(CY_ASSERT_FAILED);
    }

    if (CY_CAPSENSE_STATUS_SUCCESS == status)
    {
        /* Initialize CapSense interrupt for MSC 0 */
        Cy_SysInt_Init(&capsense_msc0_interrupt_config, capsense_msc0_isr);
        NVIC_ClearPendingIRQ(capsense_msc0_interrupt_config.intrSrc);
        NVIC_EnableIRQ(capsense_msc0_interrupt_config.intrSrc);

        /* Initialize the CapSense firmware modules. */
        status = Cy_CapSense_Enable(&cy_capsense_context);
    }

    if(status != CY_CAPSENSE_STATUS_SUCCESS)
    {
        /* This status could fail before tuning the sensors correctly.
         * Ensure that this function passes after the CapSense sensors are tuned
         * as per procedure give in the Readme.md file */
    }
}


/*******************************************************************************
* Function Name: capsense_msc0_isr
********************************************************************************
* Summary:
*  Wrapper function for handling interrupts from CapSense MSC0 block.
*
*******************************************************************************/
static void capsense_msc0_isr(void)
{
    Cy_CapSense_InterruptHandler(CY_MSC0_HW, &cy_capsense_context);
}

/*******************************************************************************
* Function Name: initialize_capsense_tuner
********************************************************************************
* Summary:
* EZI2C module to communicate with the CapSense Tuner tool.
*
*******************************************************************************/
static void initialize_capsense_tuner(void)
{
    cy_en_scb_ezi2c_status_t status = CY_SCB_EZI2C_SUCCESS;

    /* EZI2C interrupt configuration structure */
    const cy_stc_sysint_t ezi2c_intr_config =
    {
        .intrSrc = CYBSP_EZI2C_IRQ,
        .intrPriority = EZI2C_INTR_PRIORITY,
    };

    /* Initialize the EzI2C firmware module */
    status = Cy_SCB_EZI2C_Init(CYBSP_EZI2C_HW, &CYBSP_EZI2C_config, &ezi2c_context);

    if(status != CY_SCB_EZI2C_SUCCESS)
    {
        CY_ASSERT(CY_ASSERT_FAILED);
    }

    Cy_SysInt_Init(&ezi2c_intr_config, ezi2c_isr);
    NVIC_EnableIRQ(ezi2c_intr_config.intrSrc);

    /* Set the CapSense data structure as the I2C buffer to be exposed to the
     * master on primary slave address interface. Any I2C host tools such as
     * the Tuner or the Bridge Control Panel can read this buffer but you can
     * connect only one tool at a time.
     */
    Cy_SCB_EZI2C_SetBuffer1(CYBSP_EZI2C_HW, (uint8_t *)&cy_capsense_tuner,
                            sizeof(cy_capsense_tuner), sizeof(cy_capsense_tuner),
                            &ezi2c_context);

    Cy_SCB_EZI2C_Enable(CYBSP_EZI2C_HW);
}


/*******************************************************************************
* Function Name: led_control
********************************************************************************
* Summary:
* Turning LED ON/OFF based on touchpad status
*
*******************************************************************************/
// static void led_control(void)
// {
//     if(MSC_CAPSENSE_WIDGET_INACTIVE != Cy_CapSense_IsWidgetActive(CY_CAPSENSE_LINEARSLIDER0_WDGT_ID, &cy_capsense_context))
//     {
//         Cy_GPIO_Write(CYBSP_USER_LED_PORT, CYBSP_USER_LED_NUM, CYBSP_LED_STATE_ON);
//     }
//     else
//     {
//         Cy_GPIO_Write(CYBSP_USER_LED_PORT, CYBSP_USER_LED_NUM, CYBSP_LED_STATE_OFF);
//     }
// }

/*******************************************************************************
* Function Name: process_slider_position
********************************************************************************
* Summary:
* Process slider position - print position with uart / control led
*
*******************************************************************************/
static void process_slider_position(void)
{
    uint16_t slider_position;
    char print_uart[50]; // UART buffer

    /*  */
    if (CY_CAPSENSE_POSITION_NONE != Cy_CapSense_GetTouchInfo(
                                         CY_CAPSENSE_LINEARSLIDER0_WDGT_ID,
                                         &cy_capsense_context))
    {
        /* Get slider */
        slider_position = Cy_CapSense_GetTouchInfo(CY_CAPSENSE_LINEARSLIDER0_WDGT_ID,
                                                   &cy_capsense_context)->ptrPosition->x;

        /* Control LED with slider position*/
        if (slider_position < 25)
        {
            Cy_GPIO_Write(CYBSP_LED1_PORT, CYBSP_LED1_PIN, LED_OFF);
            Cy_GPIO_Write(CYBSP_LED2_PORT, CYBSP_LED2_PIN, LED_OFF);
            Cy_GPIO_Write(CYBSP_LED3_PORT, CYBSP_LED3_PIN, LED_OFF);
        }        
        else if (slider_position < 50)
        {
            Cy_GPIO_Write(CYBSP_LED1_PORT, CYBSP_LED1_PIN, LED_ON);
            Cy_GPIO_Write(CYBSP_LED2_PORT, CYBSP_LED2_PIN, LED_OFF);
            Cy_GPIO_Write(CYBSP_LED3_PORT, CYBSP_LED3_PIN, LED_OFF);
        }
        else if (slider_position < 75)
        {
            Cy_GPIO_Write(CYBSP_LED1_PORT, CYBSP_LED1_PIN, LED_ON);
            Cy_GPIO_Write(CYBSP_LED2_PORT, CYBSP_LED2_PIN, LED_ON);
            Cy_GPIO_Write(CYBSP_LED3_PORT, CYBSP_LED3_PIN, LED_OFF);
        }
        else
        {
            Cy_GPIO_Write(CYBSP_LED1_PORT, CYBSP_LED1_PIN, LED_ON);
            Cy_GPIO_Write(CYBSP_LED2_PORT, CYBSP_LED2_PIN, LED_ON);
            Cy_GPIO_Write(CYBSP_LED3_PORT, CYBSP_LED3_PIN, LED_ON);
        }

        /* Print Slider position with UART */
        // printf("Slider Position: %u\n", slider_position);
        sprintf(print_uart, "Slider Position: %u\n", slider_position);
        Cy_SCB_UART_PutString(CYBSP_UART_HW, print_uart);
    }
    else
    {
        Cy_GPIO_Write(CYBSP_LED1_PORT, CYBSP_LED1_PIN, LED_OFF);
        Cy_GPIO_Write(CYBSP_LED2_PORT, CYBSP_LED2_PIN, LED_OFF);
    }
}

/*******************************************************************************
* Function Name: ezi2c_isr
********************************************************************************
* Summary:
* Wrapper function for handling interrupts from EZI2C block.
*
*******************************************************************************/
static void ezi2c_isr(void)
{
    Cy_SCB_EZI2C_Interrupt(CYBSP_EZI2C_HW, &ezi2c_context);
}


#if CY_CAPSENSE_BIST_EN
/*******************************************************************************
 * Function Name: measure_cp
 ********************************************************************************
 * Summary:
 * Measures the sensor capacitance and shield capacitance to determine Sense clock
 * frequency. The measured sensor capacitance (Cp) values are stored in the array
 * 'sense_cap[x]', where x is the sensor number.
 *
 *******************************************************************************/
static void measure_cp(void)
{
    Cy_CapSense_SetInactiveElectrodeState(CY_CAPSENSE_SNS_CONNECTION_SHIELD,
                                CY_CAPSENSE_BIST_CSD_GROUP, &cy_capsense_context);
    for(sensor_id = CY_CAPSENSE_LINEARSLIDER0_SNS0_ID; sensor_id < NUMBER_OF_SLIDER_SEGMENTS; sensor_id++)
    {
        sensor_meas_status[sensor_id] = Cy_CapSense_MeasureCapacitanceSensorElectrode(
                                CY_CAPSENSE_LINEARSLIDER0_WDGT_ID, sensor_id, 
                                &cy_capsense_context);
        sense_cap[sensor_id] = cy_capsense_context.ptrWdConfig[CY_CAPSENSE_LINEARSLIDER0_WDGT_ID].ptrEltdCapacitance[sensor_id];
    }
    Cy_CapSense_SetInactiveElectrodeState(CY_CAPSENSE_SNS_CONNECTION_SHIELD, 
                        CY_CAPSENSE_BIST_SHIELD_GROUP, &cy_capsense_context);
    shield_meas_status = Cy_CapSense_MeasureCapacitanceShieldElectrode(SHIELD_MEAS_SKIP_CH_MASK,
                                &cy_capsense_context);
    shield_cap = cy_capsense_context.ptrBistContext->ptrChShieldCap[0];
}
#endif

/*******************************************************************************
* Function Name: initialize_uart
********************************************************************************
* Summary:
* Initialize UART
*
*******************************************************************************/
static void initialize_uart(void)
{
    Cy_SCB_UART_Init(CYBSP_UART_HW, &CYBSP_UART_config, NULL);
    Cy_SCB_UART_Enable(CYBSP_UART_HW);

    setvbuf(stdout, NULL, _IONBF, 0);
    Cy_SCB_UART_PutString(CYBSP_UART_HW, "UART Initialized\n");
}

/* [] END OF FILE */
