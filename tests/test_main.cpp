#include <iostream>

int run_binary_reader_tests();
int run_asset_tests();
int run_fli_player_tests();
int run_config_tests();
int run_recordings_tests();
int run_demo_tests();
int run_demo_story_integration_tests();
int run_sound_tests();
int run_ui_tests();
int run_winhelp_tests();
int run_help_tests();
int run_puzzle_tests();
int run_weapons_tests();
int run_level_remap_tests();
int run_object_animation_tests();
int run_scene_tests();
int run_sensor_tests();
int run_settings_menu_tests();
int run_savegame_tests();
int run_save_menu_tests();
#ifdef _WIN32
int run_platform_input_tests();
int run_midi_tests();
int run_config_storage_tests();
int run_save_storage_tests();
#endif

int main() {
    int failures = 0;
    failures += run_binary_reader_tests();
    failures += run_asset_tests();
    failures += run_fli_player_tests();
    failures += run_config_tests();
    failures += run_recordings_tests();
    failures += run_demo_tests();
    failures += run_demo_story_integration_tests();
    failures += run_sound_tests();
    failures += run_ui_tests();
    failures += run_winhelp_tests();
    failures += run_help_tests();
    failures += run_puzzle_tests();
    failures += run_weapons_tests();
    failures += run_level_remap_tests();
    failures += run_object_animation_tests();
    failures += run_settings_menu_tests();
    failures += run_scene_tests();
    failures += run_sensor_tests();
    failures += run_savegame_tests();
    failures += run_save_menu_tests();
#ifdef _WIN32
    failures += run_platform_input_tests();
    failures += run_midi_tests();
    failures += run_config_storage_tests();
    failures += run_save_storage_tests();
#endif

    if (failures != 0) {
        std::cerr << "n3d test suite failed: " << failures << " checks\n";
        return 1;
    }
    std::cout << "n3d tests passed\n";
    return 0;
}
