/*
 * This file is part of Rotorflight.
 *
 * Rotorflight is free software. You can redistribute this software
 * and/or modify this software under the terms of the GNU General
 * Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later
 * version.
 *
 * Rotorflight is distributed in the hope that they will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * SITL build of flight/servos.c (make/mcu/SITL.mk excludes the original).
 *
 * servoInit() configures the pin of each servo with the alternate function
 * of its timer. The SITL timerHardware_t has no alternateFunction field and
 * io.h declares IOConfigGPIOAF() only for STM32. The SITL has no timers and
 * no servo pins, thus servoInit() finds no servo and this code does not run.
 */

#include "platform.h"

#include "drivers/io.h"

void IOConfigGPIOAF(IO_t io, ioConfig_t cfg, uint8_t af);

#define alternateFunction output

#include "flight/servos.c"
