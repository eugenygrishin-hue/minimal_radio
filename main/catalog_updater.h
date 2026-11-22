#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UPD_IDLE = 0,
    UPD_DOWNLOADING,
    UPD_CONVERTING,
    UPD_IMPORTING,
    UPD_ASK_VALIDATE,
    UPD_VALIDATING,
    UPD_MERGING,
    UPD_DONE,
    UPD_ERROR
} catalog_updater_phase_t;

typedef struct {
    const char* cc;       // "ru", "rs"
    const char* label;    // "Россия", "Сербия"
    const char* url;      // raw.githubusercontent.com/...
} upd_source_t;

typedef struct {
    int added;
    int updated;
    int removed;
    int total_before;
    int total_after;
    int staging_count;
} updater_result_t;

// Список доступных источников
const upd_source_t* catalog_updater_get_sources(int* count);

// Запуск обновления для источника с кодом cc ("ru"/"rs")
// Возвращает false если уже идёт updater/merge/validation или нет сети
bool catalog_updater_start(const char* cc);

// Фаза / прогресс (опрашивается UI каждый кадр)
catalog_updater_phase_t catalog_updater_get_phase(void);
int      catalog_updater_get_progress(void);            // 0..100 внутри фазы
size_t   catalog_updater_get_bytes_downloaded(void);
size_t   catalog_updater_get_bytes_total(void);
int      catalog_updater_get_stations_parsed(void);
const char* catalog_updater_get_error(void);            // "" если нет ошибки

// Результат (валиден только после UPD_DONE)
bool catalog_updater_get_result(updater_result_t* out);

// Подтверждение валидации (вызывается UI в фазе ASK_VALIDATE)
// do_validate=true → B (проверить), false → A (применить сразу)
void catalog_updater_confirm_validate(bool do_validate);

// Сброс состояния после DONE/ERROR (возврат в меню)
void catalog_updater_reset(void);

// Идёт ли сейчас updater (для guard в app_main / UI)
bool catalog_updater_is_busy(void);

#ifdef __cplusplus
}
#endif
