//
//  BRRippleAccount.c
//  Core
//
//  Created by Carl Cherry on 4/16/19.
//  Copyright © 2019 Breadwinner AG. All rights reserved.
//
//  See the LICENSE file at the project root for license information.
//  See the CONTRIBUTORS file at the project root for a list of contributors.
//
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "support/BRCrypto.h"
#include "support/BRKey.h"
#include "support/BRBIP32Sequence.h"
#include "support/BRBIP39WordsEn.h"
#include "BRRipple.h"
#include "BRRippleBase.h"
#include "BRRippleAccount.h"
#include "BRRippleSignature.h"
#include "BRRippleBase58.h"

#define PRIMARY_ADDRESS_BIP44_INDEX 0

#define WORD_LIST_LENGTH 2048

struct BRRippleAccountRecord {
    BRRippleAddress raw; // The 20 byte account id

    // The public key - needed when sending 
    BRKey publicKey;  // BIP44: 'Master Public Key 'M' (264 bits) - 8

    uint32_t index;     // The BIP-44 Index used for this key.
    
    BRRippleSequence sequence;   // The NEXT valid sequence number, must be exactly 1 greater
                                 // than the last transaction sent

    BRRippleLastLedgerSequence lastLedgerSequence; // (Optional; strongly recommended) Highest ledger
                                 // index this transaction
                                 // can appear in. Specifying this field places a strict upper limit on
                                 // how long the transaction can wait to be validated or rejected.
                                 // See Reliable Transaction Submission for more details.
};

// NOTE: this is a copy from the one found in support
// TODO - modify the one in support to allow for different alphabets
size_t encodeBase58Ripple(char *str, size_t strLen, const uint8_t *data, size_t dataLen)
{
    static const char chars[] = "rpshnaf39wBUDNEGHJKLM4PQRST7VWXYZ2bcdeCg65jkm8oFqi1tuvAxyz";
    size_t i, j, len, zcount = 0;
    
    assert(data != NULL);
    while (zcount < dataLen && data && data[zcount] == 0) zcount++; // count leading zeroes
    
    uint8_t buf[(dataLen - zcount)*138/100 + 1]; // log(256)/log(58), rounded up
    
    memset(buf, 0, sizeof(buf));
    
    for (i = zcount; data && i < dataLen; i++) {
        uint32_t carry = data[i];
        
        for (j = sizeof(buf); j > 0; j--) {
            carry += (uint32_t)buf[j - 1] << 8;
            buf[j - 1] = carry % 58;
            carry /= 58;
        }
        
        var_clean(&carry);
    }
    
    i = 0;
    while (i < sizeof(buf) && buf[i] == 0) i++; // skip leading zeroes
    len = (zcount + sizeof(buf) - i) + 1;
    
    if (str && len <= strLen) {
        while (zcount-- > 0) *(str++) = chars[0];
        while (i < sizeof(buf)) *(str++) = chars[buf[i++]];
        *str = '\0';
    }
    
    mem_clean(buf, sizeof(buf));
    return (! str || len <= strLen) ? len : 0;
}

extern UInt512 getSeed(const char *paperKey)
{
    // Generate the 512bit private key using a BIP39 paperKey
    UInt512 seed = UINT512_ZERO;
    BRBIP39DeriveKey(seed.u8, paperKey, NULL); // no passphrase
    return seed;
}

extern BRKey deriveRippleKeyFromSeed (UInt512 seed, uint32_t index)
{
    BRKey privateKey;
    
    // The BIP32 privateKey for m/44'/60'/0'/0/index
    BRBIP32PrivKeyPath(&privateKey, &seed, sizeof(UInt512), 5,
                       44 | BIP32_HARD,          // purpose  : BIP-44
                       144 | BIP32_HARD,        // coin_type: Ripple
                       0 | BIP32_HARD,          // account  : <n/a>
                       0,                        // change   : not change
                       index);                   // index    :
    
    privateKey.compressed = 0;
    
    return privateKey;
}

extern char * createRippleAddressString (BRRippleAddress address, int useChecksum)
{
    char *string = calloc (1, 36);

    // The process is this:
    // 1. Prepend the Ripple address indicator (0) to the 20 bytes
    // 2. Do a douple sha265 hash on the bytes
    // 3. Use the first 4 bytes of the hash as checksum and append to the bytes
    uint8_t input[25];
    input[0] = 0; // Ripple address type
    memcpy(&input[1], address.bytes, 20);
    uint8_t hash[32];
    BRSHA256_2(hash, input, 21);
    memcpy(&input[21], hash, 4);

    // Now base58 encode the result
    encodeBase58Ripple(string, 35, input, 25);
    return string;
}

/**
 * Optional way to create the BRKey
 *
 * 1. The normal way using the mnemonic paper_key
 * 2. In DEBUG mode unit tests can pass in a private key instead
 *
 */
BRKey getKey(const char* paperKey)
{
#ifndef DEBUG
    // In release mode we assume we only support mnemonic paper key
    // Create the seed and the keys
    UInt512 seed = getSeed(paperKey);
    return deriveRippleKeyFromSeed (seed, 0);
#else
    // See if this key has any embedded spaces. If it does then
    // it is a real paper key
    int is_paper_key = 0;
    unsigned long size = strlen(paperKey);
    for(unsigned long i = 0; i < size; i++) {
        if (paperKey[i] == ' ') {
            is_paper_key = 1;
            break;
        }
    }
    if (1 == is_paper_key) {
        // Create the seed and the keys
        UInt512 seed = getSeed(paperKey);
        return deriveRippleKeyFromSeed (seed, 0);
    } else {
        BRKey key;
        BRKeySetPrivKey(&key, paperKey);
        return key;
    }
#endif
}

static BRRippleAccount createAccountObject(BRKey * key)
{
    BRRippleAccount account = (BRRippleAccount) calloc (1, sizeof (struct BRRippleAccountRecord));

    // Take a copy of the key since we are changing at least once property
    BRKey tmpKey = *key;

    // Get the public key and store with the account object
    tmpKey.compressed = 1;
    uint8_t pubkey[33];
    BRKeyPubKey(&tmpKey, pubkey, 33);
    memcpy(account->publicKey.pubKey, pubkey, 33);
    account->publicKey.compressed = 1;

    // Create the raw bytes for the 20 byte account id
    UInt160 hash = BRKeyHash160(&tmpKey);
    memcpy(account->raw.bytes, hash.u8, 20);

    return account;
}

// Create an account from the paper key
extern BRRippleAccount rippleAccountCreate (const char *paperKey)
{
    BRKey key = getKey(paperKey);

    return createAccountObject(&key);
}

// Create an account object with the seed
extern BRRippleAccount rippleAccountCreateWithSeed(UInt512 seed)
{
    BRKey key = deriveRippleKeyFromSeed (seed, 0);
    return createAccountObject(&key);
}

// Create an account object using the key
extern BRRippleAccount rippleAccountCreateWithKey(BRKey key)
{
    return createAccountObject(&key);
}

extern void rippleAccountSetSequence(BRRippleAccount account, BRRippleSequence sequence)
{
    assert(account);
    account->sequence = sequence;
}

extern void rippleAccountSetLastLedgerSequence(BRRippleAccount account,
                                               BRRippleLastLedgerSequence lastLedgerSequence)
{
    assert(account);
    account->lastLedgerSequence = lastLedgerSequence;
}

extern BRRippleAddress rippleAccountGetAddress(BRRippleAccount account)
{
    BRRippleAddress address;
    memcpy(address.bytes, account->raw.bytes, sizeof(address.bytes));
    return(address);
}

extern int rippleAccountGetAddressString(BRRippleAccount account, char * rippleAddress, int length)
{
    // Create the ripple address string
    char *address = createRippleAddressString(account->raw, 1);
    if (address) {
        int addressLength = (int)strlen(address);
        // Copy the address if we have enough room in the buffer
        if (length > addressLength) {
            strcpy(rippleAddress, address);
        }
        free(address);
        return(addressLength + 1); // string length plus terminating byte
    } else {
        return 0;
    }
}

extern BRKey rippleAccountGetPublicKey(BRRippleAccount account)
{
    // Before returning this - make sure there is no private key.
    // It should NOT be there but better to make sure.
    account->publicKey.secret = UINT256_ZERO;
    return account->publicKey;
}

extern void rippleAccountFree(BRRippleAccount account)
{
    // Currently there is not any allocated memory inside the account
    // so just delete the account itself
    free(account);
}

extern BRRippleAddress rippleAccountGetPrimaryAddress (BRRippleAccount account)
{
    // Currently we only have the primary address - so just return it
    return account->raw;
}

extern BRRippleSerializedTransaction
rippleTransactionSerializeAndSign(BRRippleTransaction transaction, BRKey *privateKey,
                                  BRKey *publicKey, uint32_t sequence, uint32_t lastLedgerSequence);

extern const BRRippleSerializedTransaction
rippleAccountSignTransaction(BRRippleAccount account, BRRippleTransaction transaction, const char *paperKey)
{
    assert(account);
    assert(transaction);
    assert(paperKey);

    // Create the private key from the paperKey
    BRKey key = getKey(paperKey);

    BRRippleSerializedTransaction signedBytes =
        rippleTransactionSerializeAndSign(transaction, &key, &account->publicKey,
                                          account->sequence, account->lastLedgerSequence);

    // Increment the sequence number if we were able to sign the bytes
    if (signedBytes) {
        account->sequence++;
    }

    return signedBytes;
}

extern BRRippleAddress
rippleAddressCreate(const char * rippleAddressString)
{
    BRRippleAddress address;
    memset(address.bytes, 0x00, sizeof(address.bytes));
    // Work backwards from this ripple address (string) to what is
    // known as the acount ID (20 bytes)
    rippleAddressStringToAddress(rippleAddressString, &address);
    return address;
}

extern int // 1 if equal
rippleAddressEqual (BRRippleAddress a1, BRRippleAddress a2) {
    return 0 == memcmp (a1.bytes, a2.bytes, 20);
}
