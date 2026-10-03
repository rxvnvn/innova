// Copyright (c) 2014 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Stealth-address narration crypter.
//
// Extracted from the retired SMSG/NYX messaging subsystem (Core Minimalization
// M1D). This class is NOT part of any messaging runtime: it is the ECDH-keyed
// AES-256-CBC crypter used by the current stealth-address narration path
// (CWallet stealth send in wallet.cpp and the Qt send dialog in
// qt/walletmodel.cpp). The class name is retained to keep the current stealth
// consumers unchanged; ownership is now wallet/stealth, not messaging.
#ifndef INNOVA_STEALTH_CRYPTER_H
#define INNOVA_STEALTH_CRYPTER_H

#include <stdint.h>
#include <vector>

class SecMsgCrypter
{
private:
    unsigned char chKey[32];
    unsigned char chIV[16];
    bool fKeySet;
public:

    SecMsgCrypter();

    ~SecMsgCrypter();

    bool SetKey(const std::vector<unsigned char>& vchNewKey, unsigned char* chNewIV);
    bool SetKey(const unsigned char* chNewKey, unsigned char* chNewIV);
    bool Encrypt(unsigned char* chPlaintext, uint32_t nPlain, std::vector<unsigned char> &vchCiphertext);
    bool Decrypt(unsigned char* chCiphertext, uint32_t nCipher, std::vector<unsigned char>& vchPlaintext);
};

#endif // INNOVA_STEALTH_CRYPTER_H
