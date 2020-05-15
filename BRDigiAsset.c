//
//  BRDigiAsset.c
//  DigiByte
//
//  Created by Yoshi Jaeger on 05.10.19.
//  Copyright © 2019 DigiByte Foundation NZ Limited. All rights reserved.
//

#include "BRDigiAsset.h"
#include "BRWallet.h"
#include "BRTransaction.h"
#include "BRArray.h"
#include <math.h>

uint8_t BRTXContainsAsset(BRTransaction *tx)
{
    return BRContainsAsset(tx->outputs, tx->outCount);
}

uint8_t BRContainsAsset(const BRTxOutput *outputs, size_t outCount)
{
    for (int p = 0; p < outCount; p++)
        if (BROutpointIsAsset(&outputs[p])) return 1;
    return 0;
}

/* (internal)
 * Tests the protocol tag of an asset.
 * Returns 1 if the test was successful, otherwise it returns zero.
 */
uint8_t BRTestProtocolTag(const uint8_t* ptr, const char* check) {
    if (ptr == NULL || check == NULL) return 0;
    return (*ptr == check[0] && *(ptr + 1) == check[1]);
}

/*
 * Returns zero if an outpoint is no asset, otherwise
 * returns the length of the asset's data
 */
uint8_t BROutpointIsAsset(const BRTxOutput* output)
{
    uint8_t* ptr;
    uint8_t length;

    if (output->scriptLen <= 6 || !output->script) return 0;
    ptr = output->script;

    if (*ptr != OP_RETURN) return 0;
    ++ptr;

    length = *ptr;
    if (length == 0 || length + 2 > output->scriptLen) return 0;
    ++ptr;

    if (!BRTestProtocolTag(ptr, "DA")) return 0;
    ++ptr;

//    if (*ptr == 0x02) return 0;
    return length;
}

typedef struct {
    uint8_t exponent;
    uint8_t byteSize;
    uint8_t mantis;
    uint8_t skipBits;
} sffcEntry;

const sffcEntry sffcTable[] = {
    { 0, 1, 5, 3 },
    { 4, 2, 9, 3 },
    { 4, 3, 17, 3 },
    { 4, 4, 25, 3 },
    { 3, 5, 34, 3 },
    { 3, 6, 42, 3 },
    { 0, 7, 54, 2 },
};

/*
 * Returns 1 if an asset was sent to the output
 */
uint8_t BROutputIsAsset(const BRTransaction* transaction, const BRTxOutput* output) {
    size_t idx = -1;
    size_t op_return_idx = -1;
    
    // Parse instruction
    BRTxOutput* or_output = NULL;
    uint8_t* ptr;
    uint8_t length;
    uint8_t type;
    uint8_t version;
    
    // 1) Check if output is part of transaction
    // 2) Search op_return output
    for (size_t i = 0; i < transaction->outCount; ++i) {
        uint8_t l;
        
        if (&transaction->outputs[i] == output)
            idx = i;
        
        l = BROutpointIsAsset(&transaction->outputs[i]);
        if (l) {
            or_output = &transaction->outputs[i];
            length = l;
        }
    }
    
    if (idx == -1) return 0;
    if (or_output == NULL) return 0;
    
    ptr = or_output->script + 4; /* OP_RETURN + LEN + TAG(2) */
    version = *ptr;
    ptr++;
    
    type = *ptr;
    
    if (!DA_IS_TRANSFER(type) && !DA_IS_BURN(type)) {
        return 0;
    }
    
    {
        // Parse the instructions
        uint16_t outputIdx;
        uint8_t inputIdx = 0;
        uint8_t skip = 0, range = 0, percent = 0;
        uint64_t amount = 0;
        uint8_t burn = 0;
        
        ++ptr;
        while (ptr - or_output->script < or_output->scriptLen - 1) {
            uint8_t flags = *ptr++;
            
            skip = !!(flags & (1 << 7));
            range = !!(flags & (1 << 6));
            percent = !!(flags & (1 << 5));
            outputIdx = flags & (~0xE0);
            if (range) {
                uint8_t outputIdx2 = *ptr++;
                // output size = 13 bits
                outputIdx = outputIdx | (outputIdx2 << 8);
            }
            
            if (percent) {
                amount = *ptr++;
            } else {
                uint8_t flagsLen = (*ptr & 0xe0) >> 5;
                if (flagsLen & 0x0F == 0x07) flagsLen = 6;
                
                assert(flagsLen <= 6 && "sffc out of range");
                sffcEntry* data = &sffcTable[flagsLen];
                amount = UInt64GetBE(ptr); // safe because scriptLen (64) is after script
                ptr += data->byteSize;
                
                // mask flag bits
                for (uint8_t mask0 = 0; mask0 < data->skipBits; ++mask0) {
                    uint64_t mask = (1UL << (63 - mask0));
                    amount &= ~mask;
                }
                
                // shift bits to the LSB
                amount >>= (64 - data->byteSize * 8);
                
                uint64_t expShift = pow(2, data->exponent);
                uint64_t exp = amount % expShift;
                uint64_t mantis = amount / expShift;
                amount = mantis * pow(10, exp);
            }
            
            burn = (outputIdx == 31 && range == 0);
            
#if DEBUG
            printf("ASSETS: skip=%d, range=%d, percent=%d, inputIdx=%d, outputIdx=%d, burn=%d, amount=%ld%s\n", skip, range, percent, inputIdx, outputIdx, burn, amount, percent ? "%" : "");
#endif
            
            // Some asset went to output-index `idx`
            if (!burn && outputIdx == idx) return 1;
            if (skip) inputIdx++;
        }
    }
    
    return 0;
}


