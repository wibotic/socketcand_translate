#pragma once

// Sends a UDP broadcast beacon to port 42000
// on the subnet where ther server was bound.
// Sets errno and returns -1 on error.
int discovery_beacon_start();
