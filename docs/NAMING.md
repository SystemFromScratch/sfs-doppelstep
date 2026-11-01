# How Doppelstep got its name

> *Your stepper, seen as a smart servo.*

## The project before the name

The project started as **sfs-stepper-driver**: the internal SystemFromScratch
name for a Zephyr RTOS firmware on the STMicroelectronics B-G431B-ESC1. It
drives a low-cost two-phase stepper motor with
[Stepper-C](https://github.com/AntonVynohradov/Stepper-C) and answers on the
bus as a DYNAMIXEL XM430-W350, through the
[DYNAMIXEL X430 Control Table Emulator](https://github.com/AntonVynohradov/DYNAMIXEL-X430-Control-Table-Emulator).
The host — ROS 2 `dynamixel_hardware_interface`, DynamixelSDK, DYNAMIXEL
Wizard — cannot tell the difference.

`sfs-stepper-driver` says what the code is. It does not say why anyone would
want it, and it reads like one more stepper driver among hundreds. An
open-source project that people should find, remember and recommend needed a
public name.

## What the name had to do

1. **Tell the story in one word.** What makes the project different is not
   "a stepper driver" but *a stepper that passes for a smart servo*: drop-in,
   no change on the host side.
2. **Stay clear of other people's trademarks.** No DYNAMIXEL, ROBOTIS or
   STM32 in the name; the name may hint at what it emulates, never borrow the
   brand.
3. **Fit the field.** Successful open motor-control projects have short,
   one-word names: ODrive, moteus, VESC, SimpleFOC.
4. **Be findable.** A word that is not already taken by a product, especially
   not in robotics, fitness or motion, so that searches lead to the project.
5. **Leave room to grow.** The technical prefix `sfs-` stays for repositories
   and identifiers of the SystemFromScratch family; the public name stands on
   its own.

## Candidates

| Name | Idea | Why not |
|---|---|---|
| Mimistep | *mimic* + step | "mimic" is worn out as a product word |
| Turncoat | the one who changes sides | fun for a talk, too flippant for production use |
| Servostep / StepServo | stepper as servo | descriptive, but very likely in use by stepper driver vendors ("easy servo" class products) |
| OpenJoint | open robot joint | generic, drowns in search results |
| Articula | articulation, joint | pleasant, but says nothing about the stepper |
| Kinestep | kinetics + step | the strongest neutral candidate — see below |
| **Doppelstep** | *Doppelgänger* + step | chosen |

## The check

On 1 November 2026 both finalists were searched on the web:

- **Kinestep:** no exact match, but two close names in a neighbouring field:
  *KinesteX*, an AI fitness trainer app, and *Kineticstep*, a crowdfunded
  walking exoskeleton. Together with "step" suggesting walking and fitness,
  searches for Kinestep would land on other products.
- **Doppelstep:** no exact match at all — no product, project or company.
  The nearest hits (a music act called *Doppelgang*, a GitHub tool named
  *DoppelSpotter*) are unrelated.

Doppelstep was clearly the cleaner and more distinctive of the two.

Not checked yet, to be done before the first public release: the GitHub
organisation name, the `.dev` / `.io` / `.com` domains, and trademark
registers (EUIPO / TMview for the EU, classes 7, 9 and 42; USPTO).

## The meaning

A **Doppelgänger** is a look-alike double: someone indistinguishable from
another person. **Doppelstep** is a stepper motor that is the double of a smart
servo: it speaks the same protocol, exposes the same Control Table, follows the
same profiles, and the controller on the other end simply sees the servo it
expects.

The name also keeps a quiet nod to its parts — *Doppel-* for the
double it plays, *-step* for the honest stepper motor underneath, driven by
Stepper-C.

## How to write it

| Where | Form |
|---|---|
| Prose, documentation, marketing | **Doppelstep** (one word, capital D) |
| Identifiers: repository, west path, Docker image and volume, CMake project | `doppelstep` |
| SystemFromScratch technical prefix (Kconfig `SFS_*`, devicetree vendor `sfs,`) | unchanged |
| Former internal name | `sfs-stepper-driver` |

Not "DoppelStep", "Doppel-Step" or "Doppelstepper".

**Tagline:** *Your stepper, seen as a smart servo.*

DYNAMIXEL and ROBOTIS are trademarks of ROBOTIS Co., Ltd. Doppelstep is an
independent project and is not affiliated with or endorsed by ROBOTIS.
