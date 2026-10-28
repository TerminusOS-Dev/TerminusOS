#ifndef CLIPBOARD_H
#define CLIPBOARD_H

// ─── Общая история буфера обмена (как Win+V в Windows) ──────────────────────
// Доступна и шеллу (текстовый режим), и gite (графический режим) — оба
// зовут clipboard_push() при копировании/вырезании и читают историю через
// clipboard_get() при вставке из панели (см. keyboard.h: CHAR_WIN_V).

#define CLIP_SLOTS   8
#define CLIP_MAXLEN  240

// Добавляет текст в начало истории (самый новый — индекс 0). Повтор подряд
// того же текста не дублируется. Слишком длинный текст обрезается.
// Каждый вызов сразу сохраняет историю на диск (clipboard.dat в корне).
void clipboard_push(const char* text);

// Удаляет одну запись (0 = самая новая) и сохраняет изменение на диск.
void clipboard_delete(int idx);

// Полностью очищает историю и сохраняет это на диск.
void clipboard_clear();

// Загружает историю с диска — вызвать один раз при старте ОС (после fat32_init()).
void clipboard_load();

// Сколько записей сейчас в истории (0..CLIP_SLOTS).
int clipboard_count();

// Текст записи по индексу (0 = самая новая). idx вне диапазона -> "".
const char* clipboard_get(int idx);

#endif
