#pragma once

#include <secp256k1_recovery.h>

extern "C"
{
#include <KeccakSponge.h>
}

#include "utils.hpp"
#include "rlp.hpp"

class Transaction
{
public:
  /**
   * @brief Transaction's fields count.
   */
  static inline constexpr std::size_t FieldsCount = 11;

  /**
   * @brief Enum containing available transaction fields.
   */
  enum Field
  {
    ChainID,
    Nonce,
    MaxPriorityFeePerGas,
    MaxFeePerGas,
    GasLimit,
    To,
    Value,
    Data,
    Parity,
    R,
    S,
  };

private:
  /**
   * @brief Enum containing available transaction fields types.
   */
  enum FieldType
  {
    DATA,
    QUANTITY,
  };

  /**
   * @brief Transaction field to its type mapping.
   */
  static inline constexpr FieldType fieldTypeMapping[FieldsCount] = {QUANTITY, QUANTITY, QUANTITY, QUANTITY, QUANTITY, DATA, QUANTITY, DATA, QUANTITY, QUANTITY, QUANTITY};

  /**
   * @brief SECP256K1 context, allows preinitialization as it's very slow to create.
   * 
   * The purpose of context structures is to cache large precomputed data tables
   * that are expensive to construct, and also to maintain the randomization data
   * for blinding.
   *
   * Do not create a new context object for each operation, as construction is
   * far slower than all other API calls (~100 times slower than an ECDSA
   * verification).
   */
  secp256k1_context *secp256k1Context;

  Utils::Byte chainID[32];
  Utils::Byte nonce[32];
  Utils::Byte maxPriorityFeePerGas[32];
  Utils::Byte maxFeePerGas[32];
  Utils::Byte gasLimit[32];
  Utils::Byte to[20];
  Utils::Byte value[32];
  Utils::Byte data[1024];
  Utils::Byte parity[32];
  Utils::Byte r[32];
  Utils::Byte s[32];

  /**
   * @brief RLP input data to encode.
   */
  RLP::Item rlpInput[FieldsCount] = {
      {.buffer = chainID, .length = 0},
      {.buffer = nonce, .length = 0},
      {.buffer = maxPriorityFeePerGas, .length = 0},
      {.buffer = maxFeePerGas, .length = 0},
      {.buffer = gasLimit, .length = 0},
      {.buffer = to, .length = 0},
      {.buffer = value, .length = 0},
      {.buffer = data, .length = 0},
      {.buffer = parity, .length = 0},
      {.buffer = r, .length = 0},
      {.buffer = s, .length = 0},
  };

  /**
   * @brief KECCAK256 hashing function.
   * 
   * @param input input buffer
   * @param inputLength input buffer length
   * @param hash output hash buffer
   * @return output buffer length (always 32)
   */
  inline std::size_t _keccak256(Utils::Buffer input, std::size_t inputLength, Utils::Buffer hash)
  {
    KeccakWidth1600_Sponge(1088, 512, input, inputLength, 0x01, hash, 32);
    return 32;
  }

  /**
   * @brief ECDSA hashing function.
   * 
   * @param hash 32 byte hash
   * @param privateKey 32 byte private key
   * @param signature output signature buffer
   * @param recid output recovery id
   * @return output signature buffer length (always 64)
   */
  inline std::size_t _ecdsa(Utils::Buffer hash, Utils::Buffer privateKey, Utils::Buffer signature, int *recid)
  {
    secp256k1_ecdsa_recoverable_signature ecdsaSig;
    secp256k1_ecdsa_sign_recoverable(secp256k1Context, &ecdsaSig, hash, privateKey, secp256k1_nonce_function_rfc6979, NULL);
    secp256k1_ecdsa_recoverable_signature_serialize_compact(secp256k1Context, signature, recid, &ecdsaSig);
    return 64;
  }

public:
  /**
   * @brief Constructs a new Transaction object.
   * Creates SECP256K1 context.
   */
  Transaction()
  {
    secp256k1Context = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
  }

  /**
   * @brief Destroys the Transaction object.
   * Destroys SECP256K1 context.
   */
  ~Transaction()
  {
    secp256k1_context_destroy(secp256k1Context);
  }

  /**
   * @brief Sets the transaction field value.
   * 
   * @param field field name
   * @param value input c-string
   */
  void setField(Field field, const char *value)
  {
    rlpInput[field].length = Utils::hexStringToBuffer(
        value,
        rlpInput[field].buffer,
        fieldTypeMapping[field] == FieldType::QUANTITY);
  }

  /**
   * @brief Sets the transaction field value.
   * 
   * @param field field name
   * @param value input buffer
   * @param size input buffer size
   */
  void setField(Field field, Buffer value, std::size_t size)
  {
    // Trim leading zero bytes if of type Quantity
    if (fieldTypeMapping[field] == FieldType::QUANTITY)
    {
      while (*value == 0 && size > 0)
      {
        ++value;
        --size;
      }
    }

    rlpInput[field].length = size;
    memcpy(rlpInput[field].buffer, value, size);
  }

  /**
   * @brief Signs transaction.
   * 
   * @param privateKey private key c-string to sign with
   * @param transaction output transaction buffer
   * 
   * @return transaction buffer length
   */
  std::size_t sign(const char *privateKey, Utils::Buffer transaction)
  {
    Utils::Byte privateKeyBuffer[32];
    Utils::hexStringToBuffer(privateKey, 64, privateKeyBuffer);
    return sign(privateKeyBuffer, transaction);
  }

  /**
   * @brief Signs transaction.
   * 
   * @param privateKey private key buffer to sign with
   * @param transaction output transaction buffer
   * 
   * @return transaction buffer length
   */
  std::size_t sign(Utils::Buffer privateKey, Utils::Buffer transaction)
  {
    // Reset signature
    rlpInput[Field::Parity].length = 0;
    rlpInput[Field::R].length = 0;
    rlpInput[Field::S].length = 0;

    // Encode transaction
    std::size_t transactionLength = RLP::encodeList(rlpInput, FieldsCount, transaction, false);

    // Wrap in EIP1559 envelope
    memmove(transaction + 1, transaction, transactionLength);
    transaction[0] = 0x02;
    ++transactionLength;

    // Get transaction hash
    Utils::Byte hash[32];
    _keccak256(transaction, transactionLength, hash);

    // Get transaction signature
    Utils::Byte signature[64];
    int recid;
    _ecdsa(hash, privateKey, signature, &recid);

    // Inject signature
    setField(Field::R, signature, 32);
    setField(Field::S, signature + 32, 32);

    // Encode signed transaction
    transactionLength = RLP::encodeList(rlpInput, FieldsCount, transaction, true);

    // Wrap in EIP1559 envelope
    memmove(transaction + 1, transaction, transactionLength);
    transaction[0] = 0x02;
    ++transactionLength;

    return transactionLength;
  }
};