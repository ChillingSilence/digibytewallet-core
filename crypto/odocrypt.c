//
//  odocrypt.c
//  DigiByte
//
//  Created by Julian Jäger on 22.07.19.
//  Copyright © 2019 DigiByte Foundation NZ Limited. All rights reserved.
//

#include "odocrypt.h"
#include "sha3/BRKeccak-800-SnP.h"

#include <assert.h>
#include <string.h>

uint64_t Rot(uint64_t x, int r)
{
    return r == 0 ? x : (x << r) ^ (x >> (64-r));
}

void OdoCrypt_Unpack(uint64_t* state, const char* bytes)
{
    memset(state, 0, STATE_SIZE * sizeof(uint64_t));
    for (int i = 0; i < STATE_SIZE; i++)
    {
        for (int j = 0; j < 8; j++)
        {
            state[i] |= (uint64_t)(uint8_t)bytes[8*i + j] << (8*j);
        }
    }
}

void OdoCrypt_PreMix(uint64_t* state)
{
    uint64_t total = 0;
    for (int i = 0; i < STATE_SIZE; i++)
        total ^= state[i];
    total ^= total >> 32;
    for (int i = 0; i < STATE_SIZE; i++)
        state[i] ^= total;
}

void OdoCrypt_ApplyMaskedSwaps(uint64_t* state, const uint64_t* mask)
{
    for (int i = 0; i < STATE_SIZE/2; i++)
    {
        uint64_t a = state[2*i];
        uint64_t b = state[2*i+1];
        // For each bit set in the mask, swap the corresponding bits in `a` and `b`
        uint64_t swp = mask[i] & (a ^ b);
        a ^= swp;
        b ^= swp;
        
        state[2*i] = a;
        state[2*i+1] = b;
    }
}

void OdoCrypt_ApplyWordShuffle(uint64_t* state, int m)
{
    uint64_t next[STATE_SIZE];
    for (int i = 0; i < STATE_SIZE; i++)
    {
        next[m*i % STATE_SIZE] = state[i];
    }
    memcpy(state, &next[0], STATE_SIZE * sizeof(uint64_t));
}

void OdoCrypt_ApplyPboxRotations(uint64_t* state, const int* rotation)
{
    for (int i = 0; i < STATE_SIZE/2; i++)
    {
        // Only rotate the even words.  Rotating the odd words wouldn't actually
        // be useful - a transformation that rotates all the words can be
        // transformed into one that only rotates the even words, then rotates
        // the odd words once after the final iteration.
        state[2*i] = Rot(state[2*i], rotation[i]);
    }
}

void OdoCrypt_ApplyPbox(uint64_t* state, const Pbox* perm)
{
    for (int i = 0; i < PBOX_SUBROUNDS-1; i++)
    {
        // Conditionally move bits between adjacent pairs of words
        OdoCrypt_ApplyMaskedSwaps(state, perm->mask[i]);
        // Move the words around
        OdoCrypt_ApplyWordShuffle(state, PBOX_M);
        // Rotate the bits within words
        OdoCrypt_ApplyPboxRotations(state, perm->rotation[i]);
    }
    OdoCrypt_ApplyMaskedSwaps(state, perm->mask[PBOX_SUBROUNDS-1]);
}

void OdoCrypt_Pack(const uint64_t* state, char* bytes)
{
    memset(bytes, 0, ODOCRYPT_DIGEST_SIZE * sizeof(char));
    for (int i = 0; i < STATE_SIZE; i++)
    {
        for (int j = 0; j < 8; j++)
        {
            bytes[8*i + j] = (state[i] >> (8*j)) & 0xff;
        }
    }
}

void OdoCrypt_ApplySboxes(uint64_t* state, const uint8_t** sbox1, const uint16_t** sbox2)
{
    const static uint64_t MASK1 = (1 << SMALL_SBOX_WIDTH) - 1;
    const static uint64_t MASK2 = (1 << LARGE_SBOX_WIDTH) - 1;
    
    int smallSboxIndex = 0;
    for (int i = 0; i < STATE_SIZE; i++)
    {
        uint64_t next = 0;
        int pos = 0;
        int largeSboxIndex = i;
        for (int j = 0; j < SMALL_SBOX_COUNT / STATE_SIZE; j++)
        {
            next |= (uint64_t) sbox1[smallSboxIndex][(state[i] >> pos) & MASK1] << pos;
            pos += SMALL_SBOX_WIDTH;
            next |= (uint64_t) sbox2[largeSboxIndex][(state[i] >> pos) & MASK2] << pos;
            pos += LARGE_SBOX_WIDTH;
            smallSboxIndex++;
        }
        state[i] = next;
    }
}

void OdoCrypt_ApplyRotations(uint64_t* state, const int* rotations)
{
    uint64_t next[STATE_SIZE];
    
    // rotate memcpy (rotate by one)
    // @Mental: Please check this. Alternatively, I would achieve it with a loop.
    // Original: std::rotate_copy(state, state+1, state+STATE_SIZE, next);
    memcpy(next, state + 1, STATE_SIZE - 1);
    next[STATE_SIZE - 1 - 1] = state[0];
    
    for (int i = 0; i < STATE_SIZE; i++)
        for (int j = 0; j < ROTATION_COUNT; j++)
        {
            next[i] ^= Rot(state[i], rotations[j]);
        }
    
    memcpy(state, next, STATE_SIZE * sizeof(uint64_t));
}

void OdoCrypt_ApplyRoundKey(uint64_t* state, int roundKey)
{
    for (int i = 0; i < STATE_SIZE; i++)
        state[i] ^= (roundKey >> i) & 1;
}

void OdoCrypt_Encrypt(OdoStruct* odo, char* cipher, const char* plain) {
    uint64_t state[STATE_SIZE];
    OdoCrypt_Unpack(state, plain);
    OdoCrypt_PreMix(state);
    for (int round = 0; round < ROUNDS; round++)
    {
        OdoCrypt_ApplyPbox(state, &odo->Permutation[0]);
        OdoCrypt_ApplySboxes(state, odo->Sbox1, odo->Sbox2);
        OdoCrypt_ApplyPbox(state, &odo->Permutation[1]);
        OdoCrypt_ApplyRotations(state, odo->Rotations);
        OdoCrypt_ApplyRoundKey(state, odo->RoundKey[round]);
    }
    OdoCrypt_Pack(state, cipher);
}

// For a standard LCG, every seed produces the same sequence, but from a different
// starting point.  This generator gives the 1st, 3rd, 6th, 10th, etc output from
// a standard LCG.  This ensures that every seed produces a unique sequence.
uint32_t OdoRandom_NextInt(OdoRandom* random)
{
    assert(random != NULL && "OdoRandom struct not initialized");
    random->addend += random->multiplicand * BASE_ADDEND;
    random->multiplicand *= BASE_MULTIPLICAND;
    random->current = random->current * random->multiplicand + random->addend;
    return random->current >> 32;
}

int OdoRandom_Next(OdoRandom* random, int N)
{
    return ((uint64_t) OdoRandom_NextInt(random) * N) >> 32;
}

uint64_t OdoRandom_NextLong(OdoRandom* random)
{
    uint64_t hi = OdoRandom_NextInt(random);
    return (hi << 32) | OdoRandom_NextInt(random);
}

void OdoRandom_Permutation8(OdoRandom* random, uint8_t* arr, size_t sz) {
    for (size_t i = 0; i < sz; i++)
        arr[i] = i;
    
    for (size_t i = 1; i < sz; i++) {
        uint8_t tmp = arr[OdoRandom_Next(random, i+1)];
        arr[OdoRandom_Next(random, i+1)] = arr[i];
        arr[i] = tmp;
    }
}

void OdoRandom_Permutation16(OdoRandom* random, uint16_t* arr, size_t sz) {
    for (size_t i = 0; i < sz; i++)
        arr[i] = i;
    
    for (size_t i = 1; i < sz; i++) {
        uint16_t tmp = arr[OdoRandom_Next(random, i+1)];
        arr[OdoRandom_Next(random, i+1)] = arr[i];
        arr[i] = tmp;
    }
}

void OdoRandom_Permutation64(OdoRandom* random, uint64_t* arr, size_t sz) {
    for (size_t i = 0; i < sz; i++)
        arr[i] = i;
    
    for (size_t i = 1; i < sz; i++) {
        uint64_t tmp = arr[OdoRandom_Next(random, i+1)];
        arr[OdoRandom_Next(random, i+1)] = arr[i];
        arr[i] = tmp;
    }
}

void Odocrypt_Init(OdoStruct* odo, uint32_t key) {
    OdoRandom r;
    r.current = key;
    r.multiplicand = 1;
    r.addend = 0;
    
    odo->key = key;
    
    // Randomize each s-box
    for (int i = 0; i < SMALL_SBOX_COUNT; i++)
    {
        OdoRandom_Permutation8(&random, odo->Sbox1[i], 1 << SMALL_SBOX_WIDTH);
    }
    for (int i = 0; i < LARGE_SBOX_COUNT; i++)
    {
        OdoRandom_Permutation16(&random, odo->Sbox2[i], 1 << LARGE_SBOX_WIDTH);
    }
    
    // Randomize each p-box
    for (int i = 0; i < 2; i++)
    {
        Pbox* perm = &odo->Permutation[i];
        for (int j = 0; j < PBOX_SUBROUNDS; j++)
            for (int k = 0; k < STATE_SIZE/2; k++)
                perm->mask[j][k] = OdoRandom_NextLong(&random);
        for (int j = 0; j < PBOX_SUBROUNDS-1; j++)
            for (int k = 0; k < STATE_SIZE/2; k++)
                perm->rotation[j][k] = OdoRandom_Next(&random, 63) + 1;
    }
    
    // Randomize rotations
    // Rotations must be distinct, non-zero, and have odd sum
    {
        int bits[WORD_BITS-1];
        OdoRandom_Permutation64(&random, bits, WORD_BITS-1);
        
        int sum = 0;
        for (int j = 0; j < ROTATION_COUNT-1; j++)
        {
            odo->Rotations[j] = bits[j] + 1;
            sum += odo->Rotations[j];
        }
        for (int j = ROTATION_COUNT-1; ; j++)
        {
            if ((bits[j] + 1 + sum) % 2)
            {
                odo->Rotations[ROTATION_COUNT-1] = bits[j] + 1;
                break;
            }
        }
    }
    
    // Randomize each round key
    for (int i = 0; i < ROUNDS; i++)
        odo->RoundKey[i] = OdoRandom_Next(&random, 1 << STATE_SIZE);
}

int Odocrypt_Hash(OdoStruct* odo, const char* pbegin, const char* pend, char* output)
{
    assert(odo != NULL && "OdoStruct must be initialized");
    char cipher[KeccakP800_stateSizeInBytes] = { '\0' };
    
    size_t len = (pend - pbegin) * sizeof(pbegin[0]);
    assert(len <= ODOCRYPT_DIGEST_SIZE);
    assert(ODOCRYPT_DIGEST_SIZE < KeccakP800_stateSizeInBytes);
    memcpy(cipher, (const void*) (&pbegin[0]), len);
    cipher[len] = 1;
    
    OdoCrypt_Encrypt(odo, cipher, cipher);
    KeccakP800_Permute_12rounds(cipher);
    memcpy(output, cipher, 32);
    
    return 1;
}
