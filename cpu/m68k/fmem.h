#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Memory buffer setup */
extern void memorySetMemory(uint8_t *memory, uint32_t size);
extern void memorySetGlobalLog(uint32_t globalLog);
extern uint8_t *memoryPointer(uint32_t address);

/* Memory access functions (big-endian) */
extern uint8_t memoryReadByte(uint32_t address);
extern uint16_t memoryReadWord(uint32_t address);
extern uint32_t memoryReadLong(uint32_t address);
extern uint64_t memoryReadLongLong(uint32_t address);
extern void memoryWriteByte(uint8_t data, uint32_t address);
extern void memoryWriteWord(uint16_t data, uint32_t address);
extern void memoryWriteLong(uint32_t data, uint32_t address);
extern void memoryWriteLongLong(uint64_t data, uint32_t address);

extern void memoryWriteLongToPointer(uint32_t data, uint8_t *address);

/* Pointer-based read macros (big-endian reads from host pointers) */
#define memoryReadByteFromPointer(address) (address[0])
#define memoryReadWordFromPointer(address) ((address[0] << 8) | address[1])
#define memoryReadLongFromPointer(address) ((address[0] << 24) | (address[1] << 16) | (address[2] << 8) | address[3])

/* Memory access logging */
typedef void (*memoryLoggingFunc)(uint32_t address, int size, int readWrite, uint32_t value);
extern void memorySetLoggingFunc(memoryLoggingFunc func);

#ifdef __cplusplus
}
#endif
