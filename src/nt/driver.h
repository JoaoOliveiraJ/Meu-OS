#pragma once

// Carrega um driver de kernel (.sys), monta o DRIVER_OBJECT e chama DriverEntry.
void driver_load(const void* image);
