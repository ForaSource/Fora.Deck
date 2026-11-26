#ifndef FORADECK_BACKEND_H
#define FORADECK_BACKEND_H

#pragma once

// Backend entry point shared by desktop integration and other consumers.

#include "Application.h"
#include "Database.h"
#include "Heartbeat.h"
#include "Manager.h"
#ifdef _WIN32
#include "Windows/RegistryScan.h"
#endif
#include "WindowManager.h"

#endif //FORADECK_BACKEND_H
