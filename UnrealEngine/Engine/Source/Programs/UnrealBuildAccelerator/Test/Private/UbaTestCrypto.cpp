// Copyright Epic Games, Inc. All Rights Reserved.

#include "UbaCrypto.h"
#include "UbaLogger.h"
#include "UbaStringBuffer.h"
#include "UbaTest.h"

namespace uba
{
	bool TestCrypto(LoggerWithWriter& logger, const StringBufferBase& rootDir)
	{
		u64 key128[] = { 0x1234567812345678llu, 0x1234567812345678llu };

		CryptoKey encryptKey = Crypto::CreateKey(logger, (const u8*)key128);

		for (u32 dataSize=1;dataSize!=135; ++dataSize)
		{
			CryptoKey decryptKey = encryptKey;

			u8* originalData = new u8[dataSize];
			for (u32 i=0;i!=dataSize; ++i)
				originalData[i] = u8(rand() % 256);

			u8* encryptedData = new u8[dataSize];
			memcpy(encryptedData, originalData, dataSize);

			for (u32 i=0; i!=3; ++i)
			{
				Guid iv;
				CHECK_TRUE(Crypto::Encrypt(logger, encryptKey, encryptedData, dataSize, iv));

				CHECK_TRUE(memcmp(encryptedData, originalData, dataSize) != 0);

				Guid iv2;
				CHECK_TRUE(Crypto::Decrypt(logger, decryptKey, encryptedData, dataSize, iv2));

				CHECK_TRUE(memcmp(encryptedData, originalData, dataSize) == 0);

				if (i == 1)
					decryptKey = Crypto::DuplicateKey(logger, encryptKey);
			}
			delete[] encryptedData;
			delete[] originalData;
			Crypto::DestroyKey(decryptKey);
		}

		Crypto::DestroyKey(encryptKey);
		return true;
	}
}