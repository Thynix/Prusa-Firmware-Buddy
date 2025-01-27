target_include_directories(firmware PRIVATE .)
add_subdirectory(include_${PRINTER})

if(BOARD STREQUAL "BUDDY")
  add_subdirectory(resolution_240x320)
endif()

if(BOARD STREQUAL "XBUDDY" OR BOARD STREQUAL "XLBUDDY")
  add_subdirectory(resolution_480x320)
endif()

target_sources(
  firmware
  PRIVATE box_unfinished_selftest.cpp
          fonts.cpp
          gcode_description.cpp
          gui_media_events.cpp
          gui_time.cpp
          guimain.cpp
          IScreenPrinting.cpp
          logger.cpp
          menu_vars.cpp
          MItem_crash.cpp
          MItem_eeprom.cpp
          MItem_experimental_tools.cpp
          MItem_filament.cpp
          MItem_hardware.cpp
          MItem_lan.cpp
          MItem_menus.cpp
          MItem_print.cpp
          MItem_tools.cpp
          network_gui_tools.cpp
          png_resources.cpp
          print_time_module.cpp
          screen_crash_recovery.cpp
          screen_dialog_does_not_exist.cpp
          screen_filebrowser.cpp
          screen_hardfault.cpp
          screen_home.cpp
          screen_menu.cpp
          screen_menu_calibration.cpp
          screen_menu_cancel_object.cpp
          screen_menu_connect.cpp
          screen_menu_eeprom.cpp
          screen_menu_eeprom_diagnostics.cpp
          screen_menu_factory_reset.cpp
          screen_menu_fail_stat.cpp
          screen_menu_filament.cpp
          screen_menu_footer_settings.cpp
          screen_menu_fw_update.cpp
          screen_menu_hardware.cpp
          screen_menu_hardware_tune.cpp
          screen_menu_hw_setup.cpp
          screen_menu_info.cpp
          screen_menu_lan_settings.cpp
          screen_menu_lang_and_time.cpp
          screen_menu_move.cpp
          screen_menu_network.cpp
          screen_menu_odometer.cpp
          screen_menu_sensor_info.cpp
          screen_menu_settings.cpp
          screen_menu_statistics.cpp
          screen_menu_steel_sheets.cpp
          screen_menu_system.cpp
          screen_menu_temperature.cpp
          screen_menu_tune.cpp
          screen_menu_user_interface.cpp
          screen_messages.cpp
          screen_print_preview.cpp
          screen_printing.cpp
          screen_prusa_link.cpp
          screen_qr_error.cpp
          screen_reset_error.cpp
          screen_sheet_rename.cpp
          screen_splash.cpp
          screen_sysinf.cpp
          screen_unknown.cpp
          screen_watchdog.cpp
          ScreenFactory.cpp
          ScreenHandler.cpp
          ScreenPrintingModel.cpp
          test_display.cpp
          window_file_list.cpp
          window_filebrowser.cpp
          window_header.cpp
          window_msgbox_wrong_printer.cpp
          window_print_progress.cpp
          window_temp_graph.cpp
          window_thumbnail.cpp
  )

if(HAS_CONTROL_MENU)
  target_sources(firmware PRIVATE screen_menu_control.cpp)
endif()

if(HAS_SERIAL_PRINT)
  target_sources(firmware PRIVATE screen_printing_serial.cpp)
endif()

if(TRANSLATIONS_ENABLED)
  target_sources(firmware PRIVATE menu_items_languages.cpp screen_menu_languages.cpp)
else()
  target_sources(firmware PRIVATE menu_items_no_languages.cpp)
endif()

if(HAS_XLCD_TOUCH_DRIVER)
  target_sources(firmware PRIVATE MItem_touch.cpp screen_touch_error.cpp)
else()
  target_sources(firmware PRIVATE no_touch.cpp)
endif()

if(HAS_TOOLCHANGER)
  target_sources(firmware PRIVATE screen_menu_filament_changeall.cpp screen_menu_tools.cpp)
endif()

if(DEBUG)
  add_subdirectory(test)
  target_sources(firmware PRIVATE screen_menu_experimental_settings_debug.cpp)
else()
  target_sources(firmware PRIVATE screen_menu_experimental_settings_release.cpp)
endif()

if(HAS_SELFTEST)
  add_subdirectory(wizard)
  target_sources(
    firmware
    PRIVATE MItem_basic_selftest.cpp
            screen_menu_diagnostics.cpp
            screen_menu_steel_sheets.cpp
            ScreenSelftest.cpp
            selftest_frame.cpp
            # steel sheets requiere first layer calibration, which require wizard, which require
            # selftest TODO add HAS_STEEL_SHEETS property
    )

  if(HAS_SELFTEST_SNAKE)
    target_sources(firmware PRIVATE screen_menu_selftest_snake.cpp)
  endif()

  if(HAS_LOADCELL)
    target_sources(firmware PRIVATE MItem_loadcell.cpp)
  endif()
else()
  target_sources(firmware PRIVATE MItem_basic_selftest_disabled.cpp)
endif()

if(HAS_MMU2)
  target_sources(
    firmware
    PRIVATE MItem_mmu.cpp
            screen_menu_filament_mmu.cpp
            screen_menu_mmu_cut_filament.cpp
            screen_menu_mmu_eject_filament.cpp
            screen_menu_mmu_fail_stats.cpp
            screen_menu_mmu_load_filament.cpp
            screen_menu_mmu_load_to_nozzle.cpp
    )
endif()

if(BOARD IN_LIST BOARDS_WITH_ST7789V)
  target_sources(firmware PRIVATE display_hw_checks_MINI.cpp version_info_ST7789V.cpp)
elseif(BOARD IN_LIST BOARDS_WITH_ILI9488)
  target_sources(firmware PRIVATE display_hw_checks_xlcd.cpp version_info_ILI9488.cpp)
endif()

add_subdirectory(dialogs)
add_subdirectory(footer)
if(HAS_LEDS)
  add_subdirectory(led_animations)
endif()
