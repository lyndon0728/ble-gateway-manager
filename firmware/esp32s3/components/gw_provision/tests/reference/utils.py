"""Byte conversion shim for the unmodified Espressif SRP Python reference."""
def bytes_to_long(value):
    return int.from_bytes(value, 'big')

def long_to_bytes(value):
    return value.to_bytes(max(1, (value.bit_length() + 7) // 8), 'big')
