//============================================================================
//
//% Colton Koop
//% 301380869
//% ckoop (ckoop@sfu.ca)
//
// File Name   : SenderY.cpp
// Version     : September 23rd, 2021
// Description : Starting point for ENSC 351 Project Part 2
// Original portions Copyright (c) 2021 Craig Scratchley  (wcs AT sfu DOT ca)
//============================================================================

#include "SenderY.h"

#include <iostream>
#include <experimental/filesystem> // for C++14
#include <filesystem>
#include <stdio.h> // for snprintf()
#include <stdint.h> // for uint8_t
#include <string.h> // for memset(), and memcpy() or strncpy()
#include <errno.h>
#include <fcntl.h>	// for O_RDWR
#include <sys/stat.h>

#include "myIO.h"
#include "VNPE.h"

using namespace std;
using namespace std::filesystem; // C++17
//using namespace experimental::filesystem; // C++14

SenderY::
SenderY(vector<const char*> iFileNames, int d)
:PeerY(d),
 bytesRd(-1), 
 fileNames(iFileNames),
 blkNum(0),
 currentState(ST_WAIT_C_1),
 fileNameIndex(0),
 EOTcounter(0)
{
}

//-----------------------------------------------------------------------------

// get rid of any characters that may have arrived from the medium.
void SenderY::dumpGlitches()
{
	const int dumpBufSz = 20;
	char buf[dumpBufSz];
	int bytesRead;
	while (dumpBufSz == (bytesRead = PE(myReadcond(mediumD, buf, dumpBufSz, 0, 0, 0))));
}

// Send the block, less the block's last byte, to the receiver.
// Returns the block's last byte.

uint8_t SenderY::sendMostBlk(blkT blkBuf)
//uint8_t SenderY::sendMostBlk(uint8_t blkBuf[BLK_SZ_CRC])
{
	const int mostBlockSize = (BLK_SZ_CRC) - 1;
	PE_NOT(myWrite(mediumD, blkBuf, mostBlockSize), mostBlockSize);
	return *(blkBuf + mostBlockSize);
}

// Send the last byte of a block to the receiver
void
SenderY::
sendLastByte(uint8_t lastByte)
{
	PE(myTcdrain(mediumD)); // wait for previous part of block to be completely drained from the descriptor
	dumpGlitches();			// dump any received glitches

	PE_NOT(myWrite(mediumD, &lastByte, sizeof(lastByte)), sizeof(lastByte));
}

/* generate a block (numbered 0) with filename and filesize */
void SenderY::genStatBlk(blkT blkBuf, const char* fileName)
//void SenderY::genStatBlk(uint8_t blkBuf[BLK_SZ_CRC], const char* fileName)
{
    blkBuf[SOH_OH] = 0;
    blkBuf[SOH_OH + 1] = ~0;
    int index = DATA_POS;
    if (*fileName) { // (0 != strcmp("", fileName)) { // (strlen(fileName) > 0)
        const auto myBasename = path( fileName ).filename().string();
        auto c_basename = myBasename.c_str();
        int fileNameLengthPlus1 = strlen(c_basename) + 1;
        // check for fileNameLengthPlus1 greater than 127.
        if (fileNameLengthPlus1 + 1 > CHUNK_SZ) { // need at least one decimal digit to store st.st_size below
            cout /* cerr */ << "Ran out of space in file info block!  Need block with 1024 bytes of data." << endl;
            exit(-1);
        }
        // On Linux: The maximum length for a file name is 255 bytes. The maximum combined length of both the file name and path name is 4096 bytes.
        memcpy(&blkBuf[index], c_basename, fileNameLengthPlus1);
        //strncpy(&blkBuf[index], c_basename, 12X);
        index += fileNameLengthPlus1;
        struct stat st;
        PE(stat(fileName, &st));
        int spaceAvailable = CHUNK_SZ + DATA_POS - index;
        int spaceNeeded = snprintf((char*)&blkBuf[index], spaceAvailable, "%ld", st.st_size); // check the value of CHUNK_SZ + DATA_POS - index
        if (spaceNeeded > spaceAvailable) {
            cout /* cerr */ << "Ran out of space in file info block!  Need block with 1024 bytes of data." << endl;
            exit(-1);
        }
        index += spaceNeeded + 1;

        blkNum = 0 - 1; // initialize blkNum for the data blocks to come.
    }
    uint8_t padSize = CHUNK_SZ + DATA_POS - index;
    memset(blkBuf+index, 0, padSize);

    // check here if index is greater than 128 or so.
    blkBuf[0] = SOH; // can be pre-initialized for efficiency if no 1K blocks allowed

    /* calculate and add CRC in network byte order */
    crc16ns((uint16_t*)&blkBuf[PAST_CHUNK], &blkBuf[DATA_POS]);
}

/* tries to generate a block.  Updates the
variable bytesRd with the number of bytes that were read
from the input file in order to create the block. Sets
bytesRd to 0 and does not actually generate a block if the end
of the input file had been reached when the previously generated block
was prepared or if the input file is empty (i.e. has 0 length).
*/
//void SenderY::genBlk(blkT blkBuf)
void SenderY::genBlk(uint8_t blkBuf[BLK_SZ_CRC])
{
	//read data and store it directly at the data portion of the buffer
	bytesRd = PE(myRead(transferringFileD, &blkBuf[DATA_POS], CHUNK_SZ ));
	if (bytesRd>0) {
		blkBuf[0] = SOH; // can be pre-initialized for efficiency
		//block number and its complement
		uint8_t nextBlkNum = blkNum + 1;
		blkBuf[SOH_OH] = nextBlkNum;
		blkBuf[SOH_OH + 1] = ~nextBlkNum;

				//pad ctrl-z for the last block
				uint8_t padSize = CHUNK_SZ - bytesRd;
				memset(blkBuf+DATA_POS+bytesRd, CTRL_Z, padSize);

			/* calculate and add CRC in network byte order */
			crc16ns((uint16_t*)&blkBuf[PAST_CHUNK], &blkBuf[DATA_POS]);
	}
}

//Send CAN_LEN copies of CAN characters in a row to the YMODEM receiver, to inform it of
//	the cancelling of a file transfer
void SenderY::cans()
{
	// No need to space in time CAN chars for Part 2.
	// This function will be more complicated in later parts. 
    char buffer[CAN_LEN];
    memset( buffer, CAN, CAN_LEN);
    PE_NOT(myWrite(mediumD, buffer, CAN_LEN), CAN_LEN);
}

/* While sending the now current block for the first time, prepare the next block if possible.
*/
// **** this function will need to be modified ****
void SenderY::sendBlkPrepNext()
{
    blkNum ++; // 1st block about to be sent or previous block ACK'd
    uint8_t lastByte = sendMostBlk(blkBuf);
    memcpy(backupBlkBuf, blkBuf, BLK_SZ_CRC);
    genBlk(blkBuf); // prepare next block
    sendLastByte(lastByte);
}

// Resends the block that had been sent previously to the xmodem receiver.
//  ***** You will have to write this simple function *****
void SenderY::resendBlk() //might be done
{
	// resend the block including the checksum or crc16
    uint8_t lastByte = sendMostBlk(backupBlkBuf);
    sendLastByte(lastByte);
}

int
SenderY::
openFileToTransfer(const char* fileName)
{
    transferringFileD = myOpen(fileName, O_RDONLY);
    return transferringFileD;
}

int
SenderY::
closeTransferredFile()
{
    return PE(myClose(transferringFileD));
}

void SenderY::sendFiles()
{
    currentState = ST_WAIT_C_1;
    EOTcounter = 0;
    const char* fileName;
    char byteToReceive;

    while(true){
        switch(currentState){
            case ST_WAIT_C_1:
                PE_NOT(myRead(mediumD, &byteToReceive, 1), 1);
                if(byteToReceive == 'C'){
                    currentState = ST_CHECKFILES;
                }
                else if(byteToReceive == CAN){
                    currentState = ST_WAIT_CAN;
                }
                else { //unexpected
                    char c = byteToReceive;
                    std::cout << "Sender received totally unexpected char #" << c << ": " << (char) c << std::endl;
                    exit(EXIT_FAILURE);
                }
            break;
            case ST_CHECKFILES://janky logic might be issues
                if((unsigned)fileNameIndex < fileNames.size()){
                    if(fileNameIndex > 0){//files opened previously need to be closed
                          closeTransferredFile();
                     }
                    fileName = fileNames[fileNameIndex];
                    if(openFileToTransfer(fileName) == -1){ //file doesn't exist
                        cans();
                        return;
                    }
                    else{ //file exists
                        genStatBlk(blkBuf, fileName);
                        sendBlkPrepNext();
                        fileNameIndex++;
                        currentState = ST_WAIT_ACKNAK_1;
                    }
                }
                else{
                    genStatBlk(blkBuf, "");
                    sendBlkPrepNext();
                    closeTransferredFile();
                    currentState = ST_WAIT_ACKNAK_3;
                }
            break;
            case ST_WAIT_ACKNAK_1:
                PE_NOT(myRead(mediumD, &byteToReceive, 1), 1);
                if(byteToReceive == ACK){
                    currentState = ST_WAIT_C_2;
                }
                else if(byteToReceive == NAK){
                    resendBlk();
                    currentState = ST_WAIT_ACKNAK_1; //same state
                }
                else if(byteToReceive == CAN){
                    currentState = ST_WAIT_CAN;
                }
                else { //unexpected
                    char c = byteToReceive;
                    std::cout << "Sender received totally unexpected char #" << c << ": " << (char) c << std::endl;
                    exit(EXIT_FAILURE);
                }
            break;
            case ST_WAIT_C_2:
                PE_NOT(myRead(mediumD, &byteToReceive, 1), 1);
                if(byteToReceive == 'C'){
                    currentState = ST_CHECKDATA;
                }
                else if(byteToReceive == CAN){
                    currentState = ST_WAIT_CAN;
                }
                else { //unexpected
                    char c = byteToReceive;
                    std::cout << "Sender received totally unexpected char #" << c << ": " << (char) c << std::endl;
                    exit(EXIT_FAILURE);
                }
            break;
            case ST_CHECKDATA://janky logic need to fix this
                //if(bytesRd <= 0 || backupBlkBuf[PAST_CHUNK-1] == CTRL_Z){//no data blocks to send
                if(bytesRd <= 0){//no data blocks to send
                    sendByte(EOT);
                    currentState = ST_WAIT_NAK;
                }
                else{
                    sendBlkPrepNext();
                    currentState = ST_WAIT_ACKNAK_2;
                }
            break;
            case ST_WAIT_ACKNAK_2:
                PE_NOT(myRead(mediumD, &byteToReceive, 1), 1);
                if(byteToReceive == ACK){
                    currentState = ST_CHECKDATA;
                }
                else if(byteToReceive == NAK){
                    resendBlk();
                    currentState = ST_WAIT_ACKNAK_2; //same state
                }
                else if(byteToReceive == CAN){
                    currentState = ST_WAIT_CAN;
                }
                else { //unexpected
                    char c = byteToReceive;
                    std::cout << "Sender received totally unexpected char #" << c << ": " << (char) c << std::endl;
                    exit(EXIT_FAILURE);
                }
            break;
            case ST_WAIT_NAK:
                PE_NOT(myRead(mediumD, &byteToReceive, 1), 1);
                if(byteToReceive == NAK){
                    sendByte(EOT);
                    EOTcounter = 1;
                    currentState = ST_WAIT_ACK;
                }
                else if(byteToReceive == CAN){
                    currentState = ST_WAIT_CAN;
                }
                else { //unexpected
                    char c = byteToReceive;
                    std::cout << "Sender received totally unexpected char #" << c << ": " << (char) c << std::endl;
                    exit(EXIT_FAILURE);
                }
            break;
            case ST_WAIT_ACK:
                PE_NOT(myRead(mediumD, &byteToReceive, 1), 1);
                if(byteToReceive == ACK){
                    EOTcounter = 0;
                    currentState = ST_WAIT_C_1;
                }
                else if(byteToReceive == NAK){
                    if(EOTcounter >= 10){
                        result = "Error: EOTcounter >= 10";
                        return;
                    }
                    else{
                        EOTcounter++;
                        currentState = ST_WAIT_ACK;//same state
                    }
                }
                else if(byteToReceive == CAN){
                    currentState = ST_WAIT_CAN;
                }
                else { //unexpected
                    char c = byteToReceive;
                    std::cout << "Sender received totally unexpected char #" << c << ": " << (char) c << std::endl;
                    exit(EXIT_FAILURE);
                }
            break;
            case ST_WAIT_ACKNAK_3:
                PE_NOT(myRead(mediumD, &byteToReceive, 1), 1);
                if(byteToReceive == ACK){
                    result = "Done";
                    return;
                }
                else if(byteToReceive == NAK){
                    //result = "success NAK";
                    //return;
                    //See Piazza questions @63 @84
                    resendBlk();
                    currentState = ST_WAIT_ACKNAK_3; //same state
                }
                else if(byteToReceive == CAN){
                    currentState = ST_WAIT_CAN;
                }
                else { //unexpected
                    char c = byteToReceive;
                    std::cout << "Sender received totally unexpected char #" << c << ": " << (char) c << std::endl;
                    exit(EXIT_FAILURE);
                }
            break;
            case ST_WAIT_CAN:
                PE_NOT(myRead(mediumD, &byteToReceive, 1), 1);
                if(byteToReceive == CAN){
                    result = "cancellation";
                    return;
                }
                else { //unexpected
                    char c = byteToReceive;
                    std::cout << "Sender received totally unexpected char #" << c << ": " << (char) c << std::endl;
                    exit(EXIT_FAILURE);
                }
            break;
        }
    }
}

/*void SenderY::sendFiles()
{
    currentState = ST_WAIT_C_1;
    //blkNum = 0; initialized in genStatBlk();
    const char* fileName = fileNames[fileNameIndex];
    openFileToTransfer(fileName);
    char byteToReceive;

    while(true){
        switch(currentState){
            case ST_WAIT_C_1:
                PE_NOT(myRead(mediumD, &byteToReceive, 1), 1);
                if(byteToReceive == 'C'){
                    genStatBlk(blkBuf, fileName);
                    sendBlkPrepNext();
                    currentState = ST_WAIT_ACKNAK_1;
                }
                else if(byteToReceive == CAN){
                    currentState = ST_WAIT_CAN;
                }
                else { //unexpected
                    char c = byteToReceive;
                    std::cout << "Sender received totally unexpected char #" << c << ": " << (char) c << std::endl;
                    exit(EXIT_FAILURE);
                }
            break;
            case ST_WAIT_ACKNAK_1:
                PE_NOT(myRead(mediumD, &byteToReceive, 1), 1);
                if(byteToReceive == ACK){
                    currentState = ST_WAIT_C_2;
                }
                else if(byteToReceive == NAK){
                    resendBlk();
                    currentState = ST_WAIT_ACKNAK_1; //same state
                }
                else if(byteToReceive == CAN){
                    currentState = ST_WAIT_CAN;
                }
                else { //unexpected
                    char c = byteToReceive;
                    std::cout << "Sender received totally unexpected char #" << c << ": " << (char) c << std::endl;
                    exit(EXIT_FAILURE);
                }
            break;
            case ST_WAIT_C_2:
                PE_NOT(myRead(mediumD, &byteToReceive, 1), 1);
                if(byteToReceive == 'C'){
                    genBlk(blkBuf);
                    if(bytesRd>0){
                        sendBlkPrepNext();
                        currentState = ST_WAIT_ACKNAK_2;
                    }
                    else{//empty file
                        sendByte(EOT);
                        currentState = ST_WAIT_NAK;
                    }
                }
                else if(byteToReceive == CAN){
                    currentState = ST_WAIT_CAN;
                }
                else { //unexpected
                    char c = byteToReceive;
                    std::cout << "Sender received totally unexpected char #" << c << ": " << (char) c << std::endl;
                    exit(EXIT_FAILURE);
                }
            break;
            case ST_WAIT_ACKNAK_2:
                PE_NOT(myRead(mediumD, &byteToReceive, 1), 1);
                if(byteToReceive == ACK){
                    currentState = ST_MOREBLKS;
                }
                else if(byteToReceive == NAK){
                    resendBlk();
                    currentState = ST_WAIT_ACKNAK_2; //same state
                }
                else if(byteToReceive == CAN){
                    currentState = ST_WAIT_CAN;
                }
                else { //unexpected
                    char c = byteToReceive;
                    std::cout << "Sender received totally unexpected char #" << c << ": " << (char) c << std::endl;
                    exit(EXIT_FAILURE);
                }
            break;
            case ST_MOREBLKS:
                if(blkBuf[PAST_CHUNK-1] == CTRL_Z){//last block
                    sendByte(EOT);
                    currentState = ST_WAIT_NAK;
                }
                else{//more blocks left
                    genBlk(blkBuf);
                    sendBlkPrepNext();
                    currentState = ST_WAIT_ACKNAK_2;
                }
            break;
            case ST_WAIT_NAK:
                PE_NOT(myRead(mediumD, &byteToReceive, 1), 1);
                if(byteToReceive == NAK){
                    sendByte(EOT);
                    currentState = ST_WAIT_ACK;
                }
                else if(byteToReceive == CAN){
                    currentState = ST_WAIT_CAN;
                }
                else { //unexpected
                    char c = byteToReceive;
                    std::cout << "Sender received totally unexpected char #" << c << ": " << (char) c << std::endl;
                    exit(EXIT_FAILURE);
                }
            break;
            case ST_WAIT_ACK:
                PE_NOT(myRead(mediumD, &byteToReceive, 1), 1);
                if(byteToReceive == ACK){
                    currentState = ST_MOREFILES;
                }
                else if(byteToReceive == CAN){
                    currentState = ST_WAIT_CAN;
                }
                else { //unexpected
                    char c = byteToReceive;
                    std::cout << "Sender received totally unexpected char #" << c << ": " << (char) c << std::endl;
                    exit(EXIT_FAILURE);
                }
            break;
            case ST_MOREFILES:
                if((unsigned)fileNameIndex < (fileNames.size()-1)){//more files to transfer
                    closeTransferredFile();
                    fileNameIndex++;
                    fileName = fileNames[fileNameIndex];
                    openFileToTransfer(fileName);
                    currentState = ST_WAIT_C_1;
                }
                else{//no more files, move to end the batch
                    currentState = ST_WAIT_C_3;
                }
            break;
            case ST_WAIT_C_3:
                PE_NOT(myRead(mediumD, &byteToReceive, 1), 1);
                if(byteToReceive == 'C'){
                    genStatBlk(blkBuf, "");
                    sendBlkPrepNext();
                    currentState = ST_WAIT_ACKNAK_3;
                }
                else if(byteToReceive == CAN){
                    currentState = ST_WAIT_CAN;
                }
                else { //unexpected
                    char c = byteToReceive;
                    std::cout << "Sender received totally unexpected char #" << c << ": " << (char) c << std::endl;
                    exit(EXIT_FAILURE);
                }
            break;
            case ST_WAIT_ACKNAK_3:
                PE_NOT(myRead(mediumD, &byteToReceive, 1), 1);
                if(byteToReceive == ACK){
                    return;
                }
                else if(byteToReceive == NAK){
                    resendBlk();
                    currentState = ST_WAIT_ACKNAK_3; //same state
                }
                else if(byteToReceive == CAN){
                    currentState = ST_WAIT_CAN;
                }
                else { //unexpected
                    char c = byteToReceive;
                    std::cout << "Sender received totally unexpected char #" << c << ": " << (char) c << std::endl;
                    exit(EXIT_FAILURE);
                }
            break;
            case ST_WAIT_CAN:
                PE_NOT(myRead(mediumD, &byteToReceive, 1), 1);
                if(byteToReceive == CAN){
                    result = "success";
                    return;
                }
                else { //unexpected
                    char c = byteToReceive;
                    std::cout << "Sender received totally unexpected char #" << c << ": " << (char) c << std::endl;
                    exit(EXIT_FAILURE);
                }
            break;
        }
    }
}*/

/*void SenderY::sendFiles()
{
    char byteToReceive;
    SenderY& ctx = *this; // needed to work with SmartState-generated code

    // ***** modify the below code according to the protocol *****
    // below is just a starting point.  You can follow a
    //  different structure if you want.

    //for (auto fileName : fileNames) {
    for (unsigned fileNameIndex = 0; fileNameIndex < fileNames.size(); ++fileNameIndex) {
        const char* fileName = fileNames[fileNameIndex];
        
        ctx.openFileToTransfer(fileName);
	
        if(ctx.transferringFileD != -1) {
            ctx.genStatBlk(blkBuf, fileName); // prepare 0eth block
        }
        PE_NOT(myRead(mediumD, &byteToReceive, 1), 1); // assuming get a 'C'
//      ctx.Crcflg = true;
    
        if(ctx.transferringFileD == -1) {
            ctx.cans();
            ctx.result = "OpenError"; // include errno and so forth in here.
        }
        else {
            ctx.sendBlkPrepNext();

            PE_NOT(myRead(mediumD, &byteToReceive, 1), 1); // assuming get an ACK
            PE_NOT(myRead(mediumD, &byteToReceive, 1), 1); // assuming get a 'C'
	
            while (ctx.bytesRd) {
                ctx.sendBlkPrepNext();
                // assuming on next line we get an ACK
                PE_NOT(myRead(mediumD, &byteToReceive, 1), 1);
            }
            ctx.sendByte(EOT); // send the first EOT
            PE_NOT(myRead(mediumD, &byteToReceive, 1), 1); // assuming get a NAK
            ctx.sendByte(EOT); // send the second EOT
            PE_NOT(myRead(mediumD, &byteToReceive, 1), 1); // assuming get an ACK

            ctx.closeTransferredFile();
        }
    }
    // indicate end of the batch.
    ctx.genStatBlk(blkBuf, ""); // prepare 0eth block
    PE_NOT(myRead(mediumD, &byteToReceive, 1), 1); // assuming get a 'C'
    ctx.sendLastByte(ctx.sendMostBlk(blkBuf));
    PE_NOT(myRead(mediumD, &byteToReceive, 1), 1); // assuming get an ACK

    ctx.result = "Done";
}*/

