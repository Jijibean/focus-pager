
# Consider dependencies only in project.
set(CMAKE_DEPENDS_IN_PROJECT_ONLY OFF)

# The set of languages for which implicit dependencies are needed:
set(CMAKE_DEPENDS_LANGUAGES
  "ASM"
  )
# The set of files for implicit dependencies of each language:
set(CMAKE_DEPENDS_CHECK_ASM
  "/Users/evanji/focus-pager/firmware/build/x509_crt_bundle.S" "/Users/evanji/focus-pager/firmware/build/esp-idf/mbedtls/CMakeFiles/__idf_mbedtls.dir/__/__/x509_crt_bundle.S.obj"
  )
set(CMAKE_ASM_COMPILER_ID "GNU")

# Preprocessor definitions for this target.
set(CMAKE_TARGET_DEFINITIONS_ASM
  "ESP_PLATFORM"
  "ESP_PSA_ITS_AVAILABLE"
  "IDF_VER=\"v6.1-dev-5572-g4288cd3334\""
  "MBEDTLS_CONFIG_FILE=\"mbedtls/esp_config.h\""
  "MBEDTLS_MAJOR_VERSION=4"
  "SOC_MMU_PAGE_SIZE=CONFIG_MMU_PAGE_SIZE"
  "SOC_XTAL_FREQ_MHZ=CONFIG_XTAL_FREQ"
  "TF_PSA_CRYPTO_USER_CONFIG_FILE=\"mbedtls/esp_config.h\""
  "_GLIBCXX_HAVE_POSIX_SEMAPHORE"
  "_GLIBCXX_USE_POSIX_SEMAPHORE"
  "_GNU_SOURCE"
  "_POSIX_READER_WRITER_LOCKS"
  "__STDC_WANT_LIB_EXT1__=0"
  )

# The include file search paths:
set(CMAKE_ASM_TARGET_INCLUDE_PATH
  "config"
  "/Users/evanji/esp/esp-idf/components/mbedtls/port/include"
  "/Users/evanji/esp/esp-idf/components/mbedtls/mbedtls/include"
  "/Users/evanji/esp/esp-idf/components/mbedtls/mbedtls/library"
  "/Users/evanji/esp/esp-idf/components/mbedtls/mbedtls/tf-psa-crypto/core"
  "/Users/evanji/esp/esp-idf/components/mbedtls/mbedtls/tf-psa-crypto/drivers/builtin/src"
  "/Users/evanji/esp/esp-idf/components/mbedtls/mbedtls/tf-psa-crypto/extras"
  "/Users/evanji/esp/esp-idf/components/mbedtls/esp_crt_bundle/include"
  "/Users/evanji/esp/esp-idf/components/mbedtls/port/psa_driver/include"
  "/Users/evanji/esp/esp-idf/components/esp_libc/platform_include"
  "/Users/evanji/esp/esp-idf/components/freertos/config/include"
  "/Users/evanji/esp/esp-idf/components/freertos/config/include/freertos"
  "/Users/evanji/esp/esp-idf/components/freertos/config/xtensa/include"
  "/Users/evanji/esp/esp-idf/components/freertos/FreeRTOS-Kernel/include"
  "/Users/evanji/esp/esp-idf/components/freertos/FreeRTOS-Kernel/portable/xtensa/include"
  "/Users/evanji/esp/esp-idf/components/freertos/FreeRTOS-Kernel/portable/xtensa/include/freertos"
  "/Users/evanji/esp/esp-idf/components/freertos/esp_additions/include"
  "/Users/evanji/esp/esp-idf/components/esp_hw_support/include"
  "/Users/evanji/esp/esp-idf/components/esp_hw_support/include/soc"
  "/Users/evanji/esp/esp-idf/components/esp_hw_support/ldo/include"
  "/Users/evanji/esp/esp-idf/components/esp_hw_support/debug_probe/include"
  "/Users/evanji/esp/esp-idf/components/esp_hw_support/etm/include"
  "/Users/evanji/esp/esp-idf/components/esp_hw_support/mspi/mspi_timing_tuning/include"
  "/Users/evanji/esp/esp-idf/components/esp_hw_support/mspi/mspi_timing_tuning/tuning_scheme_impl/include"
  "/Users/evanji/esp/esp-idf/components/esp_hw_support/mspi/mspi_intr/include"
  "/Users/evanji/esp/esp-idf/components/esp_hw_support/power_supply/include"
  "/Users/evanji/esp/esp-idf/components/esp_hw_support/modem/include"
  "/Users/evanji/esp/esp-idf/components/esp_hw_support/port/esp32/."
  "/Users/evanji/esp/esp-idf/components/esp_hw_support/port/esp32/include"
  "/Users/evanji/esp/esp-idf/components/heap/include"
  "/Users/evanji/esp/esp-idf/components/heap/tlsf"
  "/Users/evanji/esp/esp-idf/components/log/include"
  "/Users/evanji/esp/esp-idf/components/soc/include"
  "/Users/evanji/esp/esp-idf/components/soc/esp32"
  "/Users/evanji/esp/esp-idf/components/soc/esp32/include"
  "/Users/evanji/esp/esp-idf/components/soc/esp32/register"
  "/Users/evanji/esp/esp-idf/components/hal/platform_port/include"
  "/Users/evanji/esp/esp-idf/components/hal/esp32/include"
  "/Users/evanji/esp/esp-idf/components/hal/include"
  "/Users/evanji/esp/esp-idf/components/esp_rom/include"
  "/Users/evanji/esp/esp-idf/components/esp_rom/esp32/include"
  "/Users/evanji/esp/esp-idf/components/esp_rom/esp32/include/esp32"
  "/Users/evanji/esp/esp-idf/components/esp_rom/esp32"
  "/Users/evanji/esp/esp-idf/components/esp_common/include"
  "/Users/evanji/esp/esp-idf/components/esp_system/include"
  "/Users/evanji/esp/esp-idf/components/esp_system/port/soc"
  "/Users/evanji/esp/esp-idf/components/esp_system/port/include/private"
  "/Users/evanji/esp/esp-idf/components/esp_stdio/include"
  "/Users/evanji/esp/esp-idf/components/xtensa/esp32/include"
  "/Users/evanji/esp/esp-idf/components/xtensa/include"
  "/Users/evanji/esp/esp-idf/components/xtensa/deprecated_include"
  "/Users/evanji/esp/esp-idf/components/esp_hal_gpio/include"
  "/Users/evanji/esp/esp-idf/components/esp_hal_gpio/esp32/include"
  "/Users/evanji/esp/esp-idf/components/esp_hal_usb/include"
  "/Users/evanji/esp/esp-idf/components/esp_hal_pmu/include"
  "/Users/evanji/esp/esp-idf/components/esp_hal_pmu/esp32/include"
  "/Users/evanji/esp/esp-idf/components/esp_hal_regi2c/include"
  "/Users/evanji/esp/esp-idf/components/esp_hal_regi2c/esp32/include"
  "/Users/evanji/esp/esp-idf/components/esp_hal_ana_conv/include"
  "/Users/evanji/esp/esp-idf/components/esp_hal_ana_conv/esp32/include"
  "/Users/evanji/esp/esp-idf/components/esp_hal_dma/include"
  "/Users/evanji/esp/esp-idf/components/esp_hal_i2s/include"
  "/Users/evanji/esp/esp-idf/components/esp_hal_i2s/esp32/include"
  "/Users/evanji/esp/esp-idf/components/lwip/include"
  "/Users/evanji/esp/esp-idf/components/lwip/include/apps"
  "/Users/evanji/esp/esp-idf/components/lwip/lwip/src/include"
  "/Users/evanji/esp/esp-idf/components/lwip/port/include"
  "/Users/evanji/esp/esp-idf/components/lwip/port/freertos/include"
  "/Users/evanji/esp/esp-idf/components/lwip/port/esp32xx/include"
  "/Users/evanji/esp/esp-idf/components/lwip/port/esp32xx/include/arch"
  "/Users/evanji/esp/esp-idf/components/lwip/port/esp32xx/include/sys"
  "/Users/evanji/esp/esp-idf/components/esp_security/include"
  "/Users/evanji/esp/esp-idf/components/esp_hal_security/esp32/include"
  "/Users/evanji/esp/esp-idf/components/esp_hal_security/include"
  "/Users/evanji/esp/esp-idf/components/esp_pm/include"
  "/Users/evanji/esp/esp-idf/components/esp_driver_dma/include"
  "/Users/evanji/esp/esp-idf/components/mbedtls/mbedtls/tf-psa-crypto/include"
  "/Users/evanji/esp/esp-idf/components/mbedtls/mbedtls/tf-psa-crypto/drivers/builtin/include"
  "esp-idf/mbedtls/mbedtls/tf-psa-crypto/include"
  "/Users/evanji/esp/esp-idf/components/mbedtls/mbedtls/tf-psa-crypto/drivers/everest/include"
  "/Users/evanji/esp/esp-idf/components/mbedtls/mbedtls/tf-psa-crypto/drivers/p256-m/p256-m"
  "/Users/evanji/esp/esp-idf/components/mbedtls/mbedtls/tf-psa-crypto/drivers/pqcp/include"
  )

# The set of dependency files which are needed:
set(CMAKE_DEPENDS_DEPENDENCY_FILES
  "/Users/evanji/esp/esp-idf/components/mbedtls/esp_crt_bundle/esp_crt_bundle.c" "esp-idf/mbedtls/CMakeFiles/__idf_mbedtls.dir/esp_crt_bundle/esp_crt_bundle.c.obj" "gcc" "esp-idf/mbedtls/CMakeFiles/__idf_mbedtls.dir/esp_crt_bundle/esp_crt_bundle.c.obj.d"
  "/Users/evanji/esp/esp-idf/components/mbedtls/port/esp_mem.c" "esp-idf/mbedtls/CMakeFiles/__idf_mbedtls.dir/port/esp_mem.c.obj" "gcc" "esp-idf/mbedtls/CMakeFiles/__idf_mbedtls.dir/port/esp_mem.c.obj.d"
  )

# Targets to which this target links which contain Fortran sources.
set(CMAKE_Fortran_TARGET_LINKED_INFO_FILES
  )

# Targets to which this target links which contain Fortran sources.
set(CMAKE_Fortran_TARGET_FORWARD_LINKED_INFO_FILES
  )

# Fortran module output directory.
set(CMAKE_Fortran_TARGET_MODULE_DIR "")
