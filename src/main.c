#include "stm32f0xx.h"

#define app ((volatile uint32_t *)(0x08000400))
#define vec ((volatile uint32_t *)(SRAM_BASE))

typedef struct{
    uint32_t res1;
    uint32_t res2;
    uint32_t res3;
    uint32_t res4;
} info_t;

const info_t info = {0x5555, 0x6666, 0x7777, 0x8888};

uint32_t tmpKey;
uint8_t access;
uint8_t d;

void goApp(){
    for (uint8_t i = 0; i < 48; i++)
        vec[i] = app[i];

    SYSCFG->CFGR1 |= SYSCFG_CFGR1_MEM_MODE;
    //RCC->APB2ENR |= RCC_APB2ENR_SYSCFGCOMPEN;

    asm volatile(
        "msr msp, %[sp]   \n\t"
	    "bx	%[pc]         \n\t"
        :: [sp] "r" (app[0]), [pc] "r" (app[1]) 
    );
}

void flashInit(){
    if ((FLASH->CR & FLASH_CR_LOCK) != 0){
        FLASH->KEYR = FLASH_KEY1;
        FLASH->KEYR = FLASH_KEY2;
    }
}

// Очистка сектора(1КБ)
void flashSectorClear(uint32_t adr){
    FLASH->CR |= FLASH_CR_PER;
    FLASH->AR = adr;
    FLASH->CR |= FLASH_CR_STRT;

    while ((FLASH->SR & FLASH_SR_BSY) != 0);

    if ((FLASH->SR & FLASH_SR_EOP) != 0)
        FLASH->SR = FLASH_SR_EOP;

    FLASH->CR &= ~FLASH_CR_PER;
}

// Запись 2 байт данных(связанно с организацией flash) по адресу
void flashWrite(uint32_t adr, uint16_t data){
    FLASH->CR |= FLASH_CR_PG;
    *(__IO uint16_t*)(adr) = data;

    while ((FLASH->SR & FLASH_SR_BSY) != 0);

    if ((FLASH->SR & FLASH_SR_EOP) != 0)
        FLASH->SR = FLASH_SR_EOP;
    FLASH->CR &= ~FLASH_CR_PG;
}

void writeU(uint8_t data){
    *(uint8_t *)(&(CRC->DR))=data;
    while(!(USART1->ISR & USART_ISR_TXE));
    USART1->TDR=data;
}

void sendPack(uint8_t cmd, uint8_t* data, uint16_t len){
    CRC->CR=CRC_CR_RESET | CRC_CR_REV_OUT | CRC_CR_REV_IN_0;
    writeU(0x55);
    writeU(cmd);
    for(uint16_t i=0; i<len; i++) writeU(data[i]);
    uint32_t crc = CRC->DR;
    for(uint8_t i=0; i<4; i++) writeU(((uint8_t*)&crc)[i]);
    while(!(USART1->ISR & USART_ISR_TC));
}

uint8_t readU(){
    while(1){
        if(USART1->ISR & USART_ISR_RXNE){
            d = USART1->RDR;
            *(uint8_t *)(&(CRC->DR))=d;
            return 0;
        }
        if(USART1->ISR & USART_ISR_RTOF){
            USART1->ICR = USART_ICR_RTOCF;
            return 1;
        }
    }
}

void proc(){
    CRC->CR=CRC_CR_RESET | CRC_CR_REV_OUT | CRC_CR_REV_IN_0;

    uint8_t cmd=0;
    uint32_t pageN=0;
    uint8_t d32[4];
    uint8_t page[1024];
    uint32_t crcTmp;
    uint32_t crcIn;

    if(readU())return;
    if(d!=0x55)return;
    if(readU())return;

    cmd=d&0x3F;

    if(cmd == 0x04)
        for(uint32_t i=0; i<4; i++){
            if(readU())return;
            d32[i]=d;
        }

    if(cmd&0x08){
        if(readU())return;
        pageN=d;
    }
        
    if(cmd == 0x09){
        for(uint32_t i=0; i<1024; i++){
            if(readU())return;
            page[i]=d;
        }
    }
    
    crcTmp=CRC->DR;
    for(uint8_t i=0; i<4; i++){
        if(readU())return;
        ((uint8_t*)&crcIn)[i]=d;
    }
    if(crcTmp!=crcIn)return;

    if(cmd == 0) sendPack(0x40, 0, 0);
    if(cmd == 1) sendPack(0x41, (uint8_t*)&info, sizeof(info));
    if(cmd == 4){
        sendPack(0x44, 0, 0);
        if(tmpKey == 0x11223344 && *((uint32_t*)&d32) == 0x55667788){
            access=1;
            flashInit();
        }
        tmpKey = *((uint32_t*)&d32);
    }
    if(cmd == 5) sendPack(access?0x45:0xC5, 0, 0);
    
    if(cmd == 7){
        sendPack(0x47, 0, 0);
        goApp();
    }

    if(!access)return;

    if(pageN>30)sendPack(0xC0|cmd, 0, 0);

    if(cmd == 8){
        sendPack(0x48, 0, 0);
        flashSectorClear(0x08000800+pageN*1024);
        sendPack(0x88, 0, 0);
    }
    if(cmd == 9){
        sendPack(0x49, 0, 0);
        for(uint16_t i=0; i<512; i++) flashWrite((0x08000800+pageN*1024)+i*2, ((uint16_t*)&page)[i]);
        sendPack(0x89, 0, 0);
    }
    if(cmd == 10) sendPack(0x4A, (uint8_t*)(0x08000800+pageN*1024), 1024);              
    if(cmd == 11){
        CRC->CR=CRC_CR_RESET | CRC_CR_REV_OUT | CRC_CR_REV_IN_0;
        for(uint16_t i=0; i<1024; i++)*(uint8_t *)(&(CRC->DR))=*(uint8_t*)(0x08000800+pageN*1024+i);
        sendPack(0xCB, (uint8_t*)(&(CRC->DR)), 4);
    }
}

int main(void){
    RCC->AHBENR |= RCC_AHBENR_GPIOAEN | RCC_AHBENR_CRCEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN | RCC_APB2ENR_SYSCFGCOMPEN | RCC_APB2ENR_SYSCFGEN;

    GPIOA->MODER |= GPIO_MODER_MODER10_1 | GPIO_MODER_MODER9_1;
    GPIOA->AFR[1]|= 0x00000110;

    USART1->RTOR = 80;
    USART1->CR3 = USART_CR3_OVRDIS;
    USART1->CR2 = USART_CR2_RTOEN | USART_CR2_ABRMODE_Msk | USART_CR2_ABREN;
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;

    while(1){
        proc();
    } 
}

// Пакеты

// /-в мк \-из мк
// ()-ошибка
// nn-номер страницы

// Code rrcccccc
// cccccc

// 000000 - Ping
// 000001 - Информация о загрузчике
// 000010 - Информация о мк

// 000100 - Разрешить доступ
// 000101 - Проверить доступ

// 000111 - Запустить прошивку и/или Записать 4 первых байта

// 001000 - Стереть страницу
// 001001 - Записать страницу
// 001010 - Считать страницу
// 001011 - CRC страницы

// rr
// 00 - мастер
// 01 - ACK
// 10 - ACK2
// 11 - NACK

// Ping-Pong                /55 00 crc - \55 40 crc
// Информация о загрузчике  /55 01 crc - \55 41 [] crc
//// Информация о мк          /55 02 crc - \55 42 id[4] fs[2] crc

// Разрешить доступ         /55 04 [4] crc - \55 44 crc
// Проверить доступ         /55 05 crc - \55 45 crc - (\55 C5 crc)

// Записать 4 первых байта  /55 07 xx xx xx xx crc - \55 47 crc

// Стереть страницу         /55 08 nn crc - \55 48 crc - (\55 C8 crc) - \55 88 crc
// Записать страницу        /55 09 nn [1024] crc - \55 49 crc - (\55 C9 crc) - \55 89 crc
// Считать страницу         /55 0A nn crc - \55 4A [1024] crc - (\55 CA crc)
// CRC страницы             /55 0B nn crc - \55 4B [4] crc - (\55 CB crc)
