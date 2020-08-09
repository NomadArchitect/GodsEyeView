#include "../libraries/LibGUI/font.hpp"
#include "../libraries/LibGUI/gui.hpp"
#include "stdio.hpp"
#include "stdlib.hpp"

#include "GDT/gdt.hpp"
#include "Hardware/Drivers/driver.hpp"
#include "Hardware/Drivers/cmos.hpp"
#include "Hardware/Drivers/keyboard.hpp"
#include "Hardware/Drivers/mouse.hpp"
#include "Hardware/Drivers/vga.hpp"
#include "Hardware/Drivers/ata.hpp"
#include "Hardware/Drivers/amd79.hpp"
#include "Hardware/interrupts.hpp"
#include "Hardware/pci.hpp"
#include "Filesystem/fs.hpp"
#include "Filesystem/part.hpp"
#include "Filesystem/fat.hpp"

#include "multitasking.hpp"
#include "syscalls.hpp"
#include "memory.hpp"

typedef void (*constructor)();
extern "C" constructor start_ctors;
extern "C" constructor end_ctors;
extern "C" void callConstructors()
{
    for (constructor* i = &start_ctors; i != &end_ctors; i++)
        (*i)();
}

void poweroff()
{
    Port16Bit qemu_power(0x604);
    qemu_power.Write(0x2000);

    Port16Bit vbox_power(0x4004);
    vbox_power.Write(0x3400);
}

struct DriverObjects {
    MouseDriver* mouse;
    KeyboardDriver* keyboard;
} static drivers;

void desktopEnvironment()
{
    TimeDriver time;
    Graphics vga;
    GUI::Desktop desktop(640, 480, &vga, drivers.mouse, drivers.keyboard);
    vga.Init(640, 480, 16, 0x0);

    GUI::Window window(0, 0, 640, 21, 0x8, 0);
    GUI::Window *win = &window;

    GUI::Image image(10, 10, powerbutton);
    GUI::Button power_button(3, 3, 16, 15, "", poweroff);

    static uint8_t open_term_hook = 0;
    auto open_term = []() { open_term_hook = 1; };
    GUI::Button terminal_button(24, 3, 14, 15, "Terminal", open_term);

    char* user_name = "Terry";

    GUI::Label clock_label(630 - (str_len(user_name) * 8) - 80, 7, 0, 10, 0x0, 0x8, "clock");
    GUI::Label user_label(630 - (str_len(user_name) * 8), 7, 0, 10, 0x0, 0x8, user_name);

    power_button.AddImage(&image);
    win->AddWidget(&power_button);
    win->AddWidget(&terminal_button);
    win->AddWidget(&clock_label);
    win->AddWidget(&user_label);
    desktop.AddWin(1, win);

    GUI::Window terminal(22, 42, 250, 150, 0x0, 1);
    terminal.SetTitle("Terminal");

    GUI::Input TerminalInput(5, 15, 240, 20, 0xF, 0x0, "Terry@Gev>");
    GUI::Label TerminalOutput(5, 35, 100, 100, 0xF, 0x0, "");
    TerminalInput.HitboxExpand(1);

    terminal.AddWidget(&TerminalOutput);
    terminal.AddWidget(&TerminalInput);
    terminal.SetHidden(1);
    desktop.AddWin(1, &terminal);

    while (1)
    {
        desktop.Draw();
        /*Launch Application*/
        if (open_term_hook == 1){
            open_term_hook = 0;
            terminal.SetHidden(0);
        }
        else { clock_label.SetText(time.GetFullTime()); }
    }
}

void kernelInit()
{
    klog("Starting ATA driver");
    AdvancedTechnologyAttachment ata1s(true, 0x1F0);
    AdvancedTechnologyAttachment ata0s(false, 0x1F0);
    ata0s.Identify();
    char *fserror;
    /* Filesystem disabled until stable */
    //klog("Starting filesystem");
    //FileSystem fs(&ata0s, &fserror);
    //PartTable part;
    //part.ReadPartitions(&vga, &ata0s);

    DriverManager drvManager;
    klog("Starting PCI and activating drivers");
    PCIcontroller PCI;
    drvManager.AddDriver(drivers.keyboard);
    drvManager.AddDriver(drivers.mouse);
    PCI.SelectDrivers(&drvManager);
    drvManager.ActivateAll();
}

extern "C" [[noreturn]] void kernelMain(void* multiboot_structure, unsigned int magicnumber)
{
    init_serial();
    klog("Kernel started");

    GlobalDescriptorTable gdt;
    TaskManager tasksmgr;

    klog("Initializing input drivers");
    InterruptManager interrupts(0x20, &gdt, &tasksmgr);
    MouseDriver m(&interrupts, 640, 480);
    KeyboardDriver k(&interrupts);

    drivers.mouse = &m;
    drivers.keyboard = &k;
    kernelInit();

    klog("Setting up tasks");
    Task DesktopTask(&gdt, desktopEnvironment);
    tasksmgr.AppendTasks(1, &DesktopTask);
    interrupts.Activate();
    while (1);
}
