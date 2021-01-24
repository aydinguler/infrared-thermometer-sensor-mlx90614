
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// XDCtools Header files
#include <xdc/runtime/Error.h>
#include <xdc/runtime/System.h>
#include <xdc/std.h>

/* TI-RTOS Header files */
#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Swi.h>
#include <ti/sysbios/knl/Queue.h>
#include <ti/sysbios/knl/Event.h>
#include <ti/sysbios/knl/Idle.h>
#include <ti/sysbios/knl/Mailbox.h>
#include <ti/sysbios/knl/Clock.h>
#include <ti/sysbios/knl/Semaphore.h>
#include <ti/drivers/GPIO.h>
#include <ti/net/http/httpcli.h>
#include <ti/drivers/I2C.h>

#include "Board.h"

#include <sys/socket.h>
#include <arpa/inet.h>

// In this project GY-906-MLX90614ESF is connected to EK-TM4C1294-XL launchpad
//
// PD0   (launchpad) --> SCL (GY-906)
// PD1   (launchpad) --> SDA (GY-906)
// +3.3V (launchpad) --> GND (GY-906)
// GND   (launchpad) --> VCC (GY-906)

#define TIMEIP            "128.138.140.44"
#define TASKSTACKSIZE     4096
#define OUTGOING_PORT     37
#define INCOMING_PORT     5011

extern  Mailbox_Handle mailbox0;// posted by getTempTask and pended by serverSocketTask
extern  Mailbox_Handle mailbox1;// posted by updateDateTimeEverySecondTask and pended by serverSocketTask
extern  Semaphore_Handle semaphore0;// posted by Timer_ISR task and pended by getNTPTimeTask
extern  Semaphore_Handle semaphore1;// posted by serverSocketTask, getTempTask and pended by getTempTask
extern  Semaphore_Handle semaphore2;// posted by Timer_ISR task, serverSocketTask and pended by updateDateTimeEverySecondTask
int     calculatedTime;// raw1900Time is calculated
int     updatedNTPTime;// calculatedTime is updated in every second

char raw1900Time[3];

int ctr;

Void Timer_ISR(UArg arg1)
{
    Semaphore_post(semaphore2);// activate updateDateTimeEverySecondTask
    Semaphore_post(semaphore0);// activate getNTPTimeTask
}
Void updateDateTimeEverySecondTask(UArg arg1)
{
    while(1){
        // wait for the semaphore that Timer_ISR() will signal
        Semaphore_pend(semaphore2, BIOS_WAIT_FOREVER);

        calculatedTime  = raw1900Time[0]*16777216 +
                        raw1900Time[1]*65536 +
                        raw1900Time[2]*256 +
                        raw1900Time[3];

        updatedNTPTime = calculatedTime + 10800 + ctr++;

        Mailbox_post(mailbox1, &updatedNTPTime, BIOS_NO_WAIT);

        System_printf("Date: %s", ctime(&updatedNTPTime));// printed on console
        System_flush();
    }
}


Void getTempTask(UArg arg0, UArg arg1)
{
    Semaphore_pend(semaphore1, BIOS_WAIT_FOREVER);
    uint8_t        temperature;
    uint8_t         txBuffer[1];
    uint16_t         rxBuffer[2];
    I2C_Handle      i2c;
    I2C_Params      i2cParams;
    I2C_Transaction i2cTransaction;

    /* Create I2C for usage */
    I2C_Params_init(&i2cParams);
    i2cParams.bitRate = I2C_100kHz;

    while(1){

    i2c = I2C_open( Board_I2C_TMP , &i2cParams);

    if (i2c == NULL) {
        System_abort("Error Initializing I2C\n");
        System_flush();
    }

    txBuffer[0] = 0x01;
    i2cTransaction.slaveAddress = 0x5A ;
    i2cTransaction.writeBuf = txBuffer;
    i2cTransaction.writeCount = 1;
    i2cTransaction.readBuf = rxBuffer;
    i2cTransaction.readCount = 2;

        if (((I2C_transfer(i2c, &i2cTransaction))!=0)&&((rxBuffer[0]-155)>30)) {

            temperature = (rxBuffer[0]-155);
            I2C_close(i2c);
            Mailbox_post(mailbox0, &temperature, BIOS_NO_WAIT);

        }else {
            I2C_close(i2c);
            Semaphore_post(semaphore1);
        }
        I2C_close(i2c);
    }
}

void getTimeStr(char *str)
{
    // dummy get time as string function
    strcpy(str, "2021-01-07 12:34:56");
}

Void serverSocketTask(UArg arg0, UArg arg1)
{
    float sendThisTemp;
    int serverfd, new_socket, valread, len;
    struct sockaddr_in serverAddr, clientAddr;
    char buffer[30];
    char outstr[30], tmpstr[30];
    bool quit_protocol;

    serverfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverfd == -1) {
        System_printf("serverSocketTask::Socket not created.. quiting the task.\n");
        return;// just quit the tasks. nothing else to do.
    }

    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(INCOMING_PORT);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    // Attaching socket to the port
    if (bind(serverfd, (struct sockaddr *)&serverAddr,  sizeof(serverAddr))<0) {
         System_printf("serverSocketTask::bind failed..\n");

         // nothing else to do, since without bind nothing else works
         // we need to terminate the task
         return;
    }
    if (listen(serverfd, 3) < 0) {

        System_printf("serverSocketTask::listen() failed\n");
        // nothing else to do, since without bind nothing else works
        // we need to terminate the task
        return;
    }

    while(1) {

        len = sizeof(clientAddr);
        if ((new_socket = accept(serverfd, (struct sockaddr *)&clientAddr, &len))<0) {
            System_printf("serverSocketTask::accept() failed\n");
            continue;// get back to the beginning of the while loop
        }

        System_printf("Accepted connection\n"); // IP address is in clientAddr.sin_addr
        System_flush();

        // task while loop
        quit_protocol = false;
        do {

            // Receive data string
            if((valread = recv(new_socket, buffer, 10, 0))<0) {
                // there is an error. Terminate the connection and get out of the loop
                close(new_socket);
                break;
            }
            buffer[10]=0;
            if(valread<10) buffer[valread]=0;

            System_printf("message received: %s\n", buffer);

            if(!strcmp(buffer, "HELLO")) {
                strcpy(outstr," GREETINGS\n");

                // connect to SocketTest program on the system with given IP/port
                // send greetings message
                send(new_socket , outstr , strlen(outstr) , 0);
                System_printf("Server <-- GREETINGS \n");
            }
            else if(!strcmp(buffer, "GETTIME")) {
                int updatedTime;
                getTimeStr(tmpstr);
                Semaphore_post(semaphore2);// activate updateDateTimeEverySecondTask
                Mailbox_pend(mailbox1, &updatedTime, BIOS_WAIT_FOREVER);
                Mailbox_pend(mailbox1, &updatedTime, BIOS_WAIT_FOREVER);
                sprintf(outstr, "\nDATE, TIME: %s\n", ctime(&updatedTime));

                // connect to SocketTest program on the system with given IP/port
                // send date and time
                send(new_socket , outstr , strlen(outstr) , 0);
            }
            else if(!strcmp(buffer, "GETTEMP")) {

                uint8_t tempVal;
                uint8_t tempAverage = 0;
                uint16_t totalTemp=0;
                int updatedTime;
                int i;

                for (i = 0; i < 100; i++) {
                    Semaphore_post(semaphore1);
                    Mailbox_pend(mailbox0, &tempVal, BIOS_WAIT_FOREVER);
                    totalTemp += tempVal;
                }

                tempAverage = totalTemp /100;
                sendThisTemp = (float)(tempAverage);

                Semaphore_post(semaphore2);// wait for the semaphore that Timer_ISR() will signal
                Mailbox_pend(mailbox1, &updatedTime, BIOS_WAIT_FOREVER);
                Mailbox_pend(mailbox1, &updatedTime, BIOS_WAIT_FOREVER);

                sprintf(outstr, "\nDATE, TIME: %s", ctime(&updatedTime));

                // connect to SocketTest program on the system with given IP/port
                // send date and time
                send(new_socket , outstr , strlen(outstr) , 0);

                sprintf(outstr, "TEMP: %2.1f", sendThisTemp);

                // connect to SocketTest program on the system with given IP/port
                // send temp data
                send(new_socket , outstr , strlen(outstr) , 0);
                if(tempAverage > 37){
                    strcpy(outstr,"\nAlert! There is a person with abnormal body temperature.\n");
                    send(new_socket , outstr , strlen(outstr) , 0);
                }else if(tempAverage <= 37){
                    strcpy(outstr,"\nNormal body temperature.\n");
                    send(new_socket , outstr , strlen(outstr) , 0);
                }

            }
            else if(!strcmp(buffer, "QUIT")) {
                quit_protocol = true;     // it will allow us to get out of while loop
                strcpy(outstr, "\nThe program has been terminated.");

                // connect to SocketTest program on the system with given IP/port
                // send The program has been terminated message
                send(new_socket , outstr , strlen(outstr) , 0);
            }

        }
        while(!quit_protocol);

        System_flush();
        close(new_socket);
        BIOS_exit(1);
    }

    close(serverfd);
    return;
}

void timeNTP(char *serverIP, int serverPort, int *data, int size)
{
        System_printf("32 bit raw data is obtained from timeNTP\n");
        System_flush();

        int sockfd, connStat, tri;
        struct sockaddr_in serverAddr;

        sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sockfd == -1) {
            System_printf("Socket not created");
            BIOS_exit(-1);
        }
        memset(&serverAddr, 0, sizeof(serverAddr));// clear serverAddr structure
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(serverPort);// convert port # to network order
        inet_pton(AF_INET, serverIP , &(serverAddr.sin_addr));

        connStat = connect(sockfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr));// connecting….
        if(connStat < 0) {
            System_printf("sendData2Server::Error while connecting to server\n");
            if(sockfd>0) close(sockfd);
            BIOS_exit(-1);
        }

        tri = recv(sockfd, raw1900Time, sizeof(raw1900Time), 0);// receive data from server
        if(tri < 0) {
            System_printf("Error while receiving data from server\n");
            if (sockfd > 0) close(sockfd);
            BIOS_exit(-1);
        }
        if (sockfd > 0) {
            close(sockfd);
        }
}

Void getNTPTimeTask(UArg arg0, UArg arg1)
{
        // wait for the semaphore that Timer_ISR() will signal
        Semaphore_pend(semaphore0, BIOS_WAIT_FOREVER);

        GPIO_write(Board_LED0, 1); // turn on the LED

        timeNTP(TIMEIP, OUTGOING_PORT, calculatedTime, strlen(calculatedTime));

        GPIO_write(Board_LED0, 0);  // turn off the LED

}

// Tasks are created here.
bool createTasks(void)
{
    static Task_Handle taskHandle1, taskHandle2, taskHandle3, taskHandle4;
    Task_Params taskParams;
    Error_Block eb;

    Error_init(&eb);

    Task_Params_init(&taskParams);
    taskParams.stackSize = TASKSTACKSIZE;
    taskParams.priority = 1;
    taskHandle1 = Task_create((Task_FuncPtr)updateDateTimeEverySecondTask, &taskParams, &eb);

    Task_Params_init(&taskParams);
    taskParams.stackSize = TASKSTACKSIZE;
    taskParams.priority = 1;
    taskHandle2 = Task_create((Task_FuncPtr)serverSocketTask, &taskParams, &eb);

    Task_Params_init(&taskParams);
    taskParams.stackSize = TASKSTACKSIZE;
    taskParams.priority = 1;
    taskHandle3 = Task_create((Task_FuncPtr)getTempTask, &taskParams, &eb);

    Task_Params_init(&taskParams);
    taskParams.stackSize = TASKSTACKSIZE;
    taskParams.priority = 1;
    taskHandle4 = Task_create((Task_FuncPtr)getNTPTimeTask, &taskParams, &eb);

    if (taskHandle1 == NULL ||  taskHandle2 == NULL || taskHandle3 == NULL || taskHandle4 == NULL) {
        printError("netIPAddrHook: Failed to create HTTP, Socket and Server Tasks\n", -1);
        return false;
    }else{
        return true;
    }

}

// printError
void printError(char *errString, int code)
{
    System_printf("Error! code = %d, desc = %s\n", code, errString);
    BIOS_exit(code);
}

void netIPAddrHook(unsigned int fAdd)
{
    // Start creating tasks
    if (fAdd) {
        createTasks();
    }
}


int main(void)

{
    /* Call board init functions */
    Board_initGeneral();
    Board_initGPIO();
    Board_initEMAC();
    Board_initI2C();

    /* Turn on user LED */
    GPIO_write(Board_LED0, Board_LED_ON);

    System_printf("Starting the HTTP GET example\nSystem provider is set to "
            "SysMin. Halt the target to view any SysMin contents in ROV.\n");
    /* SysMin will only print to the console when you call flush or exit */
    System_flush();


    /* Start BIOS */
    BIOS_start();

    return (0);
}
