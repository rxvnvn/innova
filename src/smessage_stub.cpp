
#include "smessage.h"

#include <openssl/evp.h>
#include <openssl/aes.h>
#include <cstdio>
#include <ctime>
#include <cstring>

bool fSecMsgEnabled = false;

bool SecMsgCrypter::SetKey(const std::vector<unsigned char>& vchNewKey, unsigned char* chNewIV)
{
    if (vchNewKey.size() < sizeof(chKey))
        return false;
    return SetKey(&vchNewKey[0], chNewIV);
}

bool SecMsgCrypter::SetKey(const unsigned char* chNewKey, unsigned char* chNewIV)
{
    memcpy(&chKey[0], chNewKey, sizeof(chKey));
    memcpy(chIV, chNewIV, sizeof(chIV));
    fKeySet = true;
    return true;
}

bool SecMsgCrypter::Encrypt(unsigned char* chPlaintext, uint32_t nPlain, std::vector<unsigned char>& vchCiphertext)
{
    if (!fKeySet)
        return false;

    int nLen = nPlain;
    int nCLen = nLen + AES_BLOCK_SIZE, nFLen = 0;
    vchCiphertext = std::vector<unsigned char>(nCLen);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return false;

    bool fOk = true;
    if (fOk) fOk = EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, &chKey[0], &chIV[0]);
    if (fOk) fOk = EVP_EncryptUpdate(ctx, &vchCiphertext[0], &nCLen, chPlaintext, nLen);
    if (fOk) fOk = EVP_EncryptFinal_ex(ctx, (&vchCiphertext[0]) + nCLen, &nFLen);
    EVP_CIPHER_CTX_free(ctx);

    if (!fOk)
        return false;

    vchCiphertext.resize(nCLen + nFLen);
    return true;
}

bool SecMsgCrypter::Decrypt(unsigned char* chCiphertext, uint32_t nCipher, std::vector<unsigned char>& vchPlaintext)
{
    if (!fKeySet)
        return false;

    int nPLen = nCipher, nFLen = 0;
    vchPlaintext.resize(nCipher);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return false;

    bool fOk = true;
    if (fOk) fOk = EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, &chKey[0], &chIV[0]);
    if (fOk) fOk = EVP_DecryptUpdate(ctx, &vchPlaintext[0], &nPLen, chCiphertext, nCipher);
    if (fOk) fOk = EVP_DecryptFinal_ex(ctx, (&vchPlaintext[0]) + nPLen, &nFLen);
    EVP_CIPHER_CTX_free(ctx);

    if (!fOk)
        return false;

    vchPlaintext.resize(nPLen + nFLen);
    return true;
}

bool SecureMsgStart(bool, bool)
{
    fSecMsgEnabled = false;
    return true;
}

bool SecureMsgShutdown()
{
    fSecMsgEnabled = false;
    return true;
}

bool SecureMsgEnable()
{
    fSecMsgEnabled = false;
    return true;
}

bool SecureMsgDisable()
{
    fSecMsgEnabled = false;
    return true;
}

bool SecureMsgReceiveData(CNode*, std::string, CDataStream&)
{
    return false;
}

bool SecureMsgSendData(CNode*, bool)
{
    return false;
}

bool SecureMsgScanBlock(CBlock&)
{
    return false;
}

bool SecureMsgScanBlockChain()
{
    return false;
}

bool SecureMsgScanBuckets()
{
    return false;
}

int SecureMsgWalletUnlocked()
{
    return 0;
}

int SecureMsgWalletKeyChanged(std::string, std::string, ChangeType)
{
    return 0;
}

int SecureMsgAddAddress(std::string&, std::string&)
{
    return 0;
}

int SecureMsgSend(std::string&, std::string&, std::string&, std::string&)
{
    return 0;
}

int SecureMsgDecrypt(bool, std::string&, unsigned char*, unsigned char*, uint32_t, MessageData&)
{
    return 0;
}

int SecureMsgDecrypt(bool, std::string&, SecureMessage&, MessageData&)
{
    return 0;
}

std::string getTimeString(int64_t timestamp, char *buffer, size_t nBuffer)
{
    struct tm* dt;
    time_t t = timestamp;
    dt = localtime(&t);
    strftime(buffer, nBuffer, "%Y-%m-%d %H:%M:%S %z", dt);
    return std::string(buffer);
}
