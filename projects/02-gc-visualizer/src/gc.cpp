/*
 * Mark-Sweep Garbage Collector Visualizer
 * A toy GC with real-time heap visualization
 * Demonstrates allocation, marking, sweeping, fragmentation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/bind.h>
#include <string>
#include <vector>
#endif

#define HEAP_SIZE 256
#define MAX_ROOTS 32
#define MAX_OBJECTS 128
#define MAX_EDGES 256

/* ============================================================
 * Object & Heap Types
 * ============================================================ */

typedef enum {
    OBJ_INT,
    OBJ_PAIR,
    OBJ_ARRAY,
    OBJ_STRING
} ObjectType;

typedef enum {
    COLOR_WHITE,  // unmarked (will be swept)
    COLOR_GRAY,   // discovered but children not scanned
    COLOR_BLACK   // fully marked
} GCColor;

typedef enum {
    PHASE_IDLE,
    PHASE_ALLOCATING,
    PHASE_MARK_INIT,
    PHASE_MARK_GRAY,
    PHASE_MARK_SCAN,
    PHASE_SWEEP,
    PHASE_DONE
} GCPhase;

typedef struct Object {
    int id;
    ObjectType type;
    GCColor color;
    int size;           // bytes used
    bool alive;
    int generation;

    // For pair type
    struct Object* head;
    struct Object* tail;

    // For array type
    struct Object* elements[8];
    int num_elements;

    // For string
    char str_value[64];

    // For int
    int int_value;

    // Position in heap for visualization
    int heap_offset;
} Object;

typedef struct {
    Object objects[MAX_OBJECTS];
    int num_objects;

    Object* roots[MAX_ROOTS];
    int num_roots;

    int heap_used;
    int heap_total;
    int gc_threshold;
    int total_allocated;
    int total_freed;
    int gc_count;
    int generation;

    GCPhase phase;
    int phase_step;

    // Gray set for incremental marking
    Object* gray_set[MAX_OBJECTS];
    int gray_count;

    // History for visualization
    char event_log[4096];
    int log_len;
} GCHeap;

static GCHeap heap;

/* ============================================================
 * Logging
 * ============================================================ */

static void gc_log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(heap.event_log + heap.log_len,
                      sizeof(heap.event_log) - heap.log_len, fmt, args);
    va_end(args);
    if (n > 0) heap.log_len += n;
    // Add newline
    if (heap.log_len < (int)sizeof(heap.event_log) - 2) {
        heap.event_log[heap.log_len++] = '\n';
        heap.event_log[heap.log_len] = '\0';
    }
}

static void clear_log() {
    heap.event_log[0] = '\0';
    heap.log_len = 0;
}

/* ============================================================
 * Heap Initialization
 * ============================================================ */

static void heap_init() {
    memset(&heap, 0, sizeof(heap));
    heap.heap_total = HEAP_SIZE;
    heap.gc_threshold = HEAP_SIZE * 3 / 4;
    heap.phase = PHASE_IDLE;
    gc_log("=== GC Visualizer Initialized ===");
    gc_log("Heap size: %d bytes", HEAP_SIZE);
    gc_log("GC threshold: %d bytes", heap.gc_threshold);
}

/* ============================================================
 * Object Allocation
 * ============================================================ */

static Object* alloc_object(ObjectType type, int size) {
    if (heap.num_objects >= MAX_OBJECTS) {
        gc_log("ERROR: Max objects reached");
        return NULL;
    }

    Object* obj = &heap.objects[heap.num_objects++];
    memset(obj, 0, sizeof(Object));
    obj->id = heap.num_objects;
    obj->type = type;
    obj->color = COLOR_WHITE;
    obj->size = size;
    obj->alive = true;
    obj->generation = heap.generation;
    obj->heap_offset = heap.heap_used;

    heap.heap_used += size;
    heap.total_allocated += size;

    return obj;
}

/* ============================================================
 * Object Creation API
 * ============================================================ */

static Object* gc_alloc_int(int value) {
    if (heap.heap_used + 16 > heap.gc_threshold) {
        gc_log("⚠ Threshold reached, triggering GC...");
        // We'll handle GC separately
    }
    Object* obj = alloc_object(OBJ_INT, 16);
    if (obj) {
        obj->int_value = value;
        gc_log("+ Allocated INT[%d] = %d (%d bytes)", obj->id, value, 16);
    }
    return obj;
}

static Object* gc_alloc_pair(Object* a, Object* b) {
    Object* obj = alloc_object(OBJ_PAIR, 24);
    if (obj) {
        obj->head = a;
        obj->tail = b;
        obj->num_elements = 2;
        if (a) obj->elements[0] = a;
        if (b) obj->elements[1] = b;
        gc_log("+ Allocated PAIR[%d] → [%d, %d] (%d bytes)",
               obj->id, a ? a->id : 0, b ? b->id : 0, 24);
    }
    return obj;
}

static Object* gc_alloc_array(int count) {
    Object* obj = alloc_object(OBJ_ARRAY, 16 + count * 4);
    if (obj) {
        obj->num_elements = count;
        gc_log("+ Allocated ARRAY[%d] size=%d (%d bytes)", obj->id, count, 16 + count * 4);
    }
    return obj;
}

static void array_set(Object* arr, int index, Object* val) {
    if (arr && index >= 0 && index < arr->num_elements) {
        arr->elements[index] = val;
    }
}

static Object* gc_alloc_string(const char* s) {
    Object* obj = alloc_object(OBJ_STRING, 32);
    if (obj) {
        strncpy(obj->str_value, s, 63);
        gc_log("+ Allocated STRING[%d] = \"%s\" (%d bytes)", obj->id, s, 32);
    }
    return obj;
}

/* ============================================================
 * Root Management
 * ============================================================ */

static void gc_add_root(Object* obj) {
    if (obj && heap.num_roots < MAX_ROOTS) {
        heap.roots[heap.num_roots++] = obj;
        gc_log("★ Root added: [%d]", obj->id);
    }
}

static void gc_remove_root(int index) {
    if (index >= 0 && index < heap.num_roots) {
        gc_log("☆ Root removed: [%d]", heap.roots[index]->id);
        for (int i = index; i < heap.num_roots - 1; i++) {
            heap.roots[i] = heap.roots[i + 1];
        }
        heap.num_roots--;
    }
}

/* ============================================================
 * Mark Phase
 * ============================================================ */

static void mark_gray(Object* obj) {
    if (!obj || obj->color != COLOR_WHITE) return;
    obj->color = COLOR_GRAY;
    if (heap.gray_count < MAX_OBJECTS) {
        heap.gray_set[heap.gray_count++] = obj;
    }
}

static void mark_all_roots() {
    gc_log("--- Mark Phase: scanning roots ---");
    for (int i = 0; i < heap.num_roots; i++) {
        mark_gray(heap.roots[i]);
        gc_log("  Root [%d] → gray", heap.roots[i]->id);
    }
}

static void scan_black(Object* obj) {
    if (!obj) return;
    obj->color = COLOR_BLACK;
    // Mark children gray
    for (int i = 0; i < obj->num_elements; i++) {
        if (obj->elements[i]) {
            if (obj->elements[i]->color == COLOR_WHITE) {
                mark_gray(obj->elements[i]);
            }
        }
    }
}

static bool mark_step() {
    if (heap.gray_count == 0) return true; // done

    // Process one gray object per step (for visualization)
    Object* obj = heap.gray_set[--heap.gray_count];
    scan_black(obj);
    gc_log("  [%d] gray → black (scanned %d children)",
           obj->id, obj->num_elements);
    return heap.gray_count == 0;
}

/* ============================================================
 * Sweep Phase
 * ============================================================ */

static void sweep_all() {
    gc_log("--- Sweep Phase ---");
    int freed = 0;
    int freed_bytes = 0;
    for (int i = 0; i < heap.num_objects; i++) {
        Object* obj = &heap.objects[i];
        if (!obj->alive) continue;

        if (obj->color == COLOR_WHITE) {
            obj->alive = false;
            freed++;
            freed_bytes += obj->size;
            heap.heap_used -= obj->size;
            heap.total_freed += obj->size;
            gc_log("  ✕ Swept [%d] type=%d (%d bytes)", obj->id, obj->type, obj->size);
        } else {
            // Reset color for next cycle
            obj->color = COLOR_WHITE;
        }
    }
    gc_log("Swept %d objects, freed %d bytes", freed, freed_bytes);
    gc_log("Heap: %d/%d bytes used", heap.heap_used, heap.heap_total);
    heap.gc_count++;
    heap.generation++;
}

/* ============================================================
 * Full GC Cycle
 * ============================================================ */

static void gc_collect() {
    clear_log();
    gc_log("═══════════════════════════════");
    gc_log("  GC Cycle #%d", heap.gc_count + 1);
    gc_log("═══════════════════════════════");
    gc_log("Before: %d objects, %d bytes used", heap.num_objects, heap.heap_used);

    // Mark
    heap.gray_count = 0;
    mark_all_roots();

    // Process all gray objects
    while (heap.gray_count > 0) {
        mark_step();
    }

    // Sweep
    sweep_all();

    gc_log("After: %d bytes used", heap.heap_used);
    gc_log("═══════════════════════════════");
}

/* ============================================================
 * Visualization Data (JSON)
 * ============================================================ */

#ifdef __EMSCRIPTEN__

static std::string get_heap_state() {
    std::string json = "{\"objects\":[";

    bool first = true;
    for (int i = 0; i < heap.num_objects; i++) {
        Object* obj = &heap.objects[i];
        if (!first) json += ",";
        first = false;

        json += "{";
        json += "\"id\":" + std::to_string(obj->id) + ",";
        json += "\"type\":" + std::to_string(obj->type) + ",";
        json += "\"color\":" + std::to_string(obj->color) + ",";
        json += "\"size\":" + std::to_string(obj->size) + ",";
        json += "\"alive\":" + std::string(obj->alive ? "true" : "false") + ",";
        json += "\"gen\":" + std::to_string(obj->generation) + ",";
        json += "\"offset\":" + std::to_string(obj->heap_offset) + ",";
        json += "\"value\":";
        switch (obj->type) {
            case OBJ_INT: json += std::to_string(obj->int_value); break;
            case OBJ_STRING: json += "\"" + std::string(obj->str_value) + "\""; break;
            case OBJ_PAIR: json += "\"pair\""; break;
            case OBJ_ARRAY: json += "\"array[" + std::to_string(obj->num_elements) + "]\""; break;
            default: json += "null";
        }
        json += ",\"children\":[";
        for (int j = 0; j < obj->num_elements; j++) {
            if (j > 0) json += ",";
            json += obj->elements[j] ? std::to_string(obj->elements[j]->id) : "0";
        }
        json += "]}";
    }
    json += "],\"roots\":[";
    for (int i = 0; i < heap.num_roots; i++) {
        if (i > 0) json += ",";
        json += std::to_string(heap.roots[i]->id);
    }
    json += "],\"stats\":{";
    json += "\"heap_used\":" + std::to_string(heap.heap_used) + ",";
    json += "\"heap_total\":" + std::to_string(heap.heap_total) + ",";
    json += "\"num_objects\":" + std::to_string(heap.num_objects) + ",";
    json += "\"total_allocated\":" + std::to_string(heap.total_allocated) + ",";
    json += "\"total_freed\":" + std::to_string(heap.total_freed) + ",";
    json += "\"gc_count\":" + std::to_string(heap.gc_count) + ",";
    json += "\"generation\":" + std::to_string(heap.generation);
    json += "}}";
    return json;
}

static std::string get_log() {
    return std::string(heap.event_log);
}

static int create_int(int val) {
    Object* o = gc_alloc_int(val);
    return o ? o->id : 0;
}

static int create_pair(int a_id, int b_id) {
    Object* a = NULL, *b = NULL;
    for (int i = 0; i < heap.num_objects; i++) {
        if (heap.objects[i].id == a_id && heap.objects[i].alive) a = &heap.objects[i];
        if (heap.objects[i].id == b_id && heap.objects[i].alive) b = &heap.objects[i];
    }
    Object* o = gc_alloc_pair(a, b);
    return o ? o->id : 0;
}

static int create_string(const std::string& s) {
    Object* o = gc_alloc_string(s.c_str());
    return o ? o->id : 0;
}

static int create_array(int count) {
    Object* o = gc_alloc_array(count);
    return o ? o->id : 0;
}

static void set_array_element(int arr_id, int idx, int val_id) {
    for (int i = 0; i < heap.num_objects; i++) {
        if (heap.objects[i].id == arr_id && heap.objects[i].alive) {
            Object* val = NULL;
            for (int j = 0; j < heap.num_objects; j++) {
                if (heap.objects[j].id == val_id && heap.objects[j].alive) {
                    val = &heap.objects[j]; break;
                }
            }
            array_set(&heap.objects[i], idx, val);
            break;
        }
    }
}

static void add_root_by_id(int id) {
    for (int i = 0; i < heap.num_objects; i++) {
        if (heap.objects[i].id == id && heap.objects[i].alive) {
            gc_add_root(&heap.objects[i]);
            return;
        }
    }
}

static void remove_root_by_id(int id) {
    for (int i = 0; i < heap.num_roots; i++) {
        if (heap.roots[i]->id == id) {
            gc_remove_root(i);
            return;
        }
    }
}

static void run_gc() {
    gc_collect();
}

static void reset_heap() {
    heap_init();
}

EMSCRIPTEN_BINDINGS(gc_module) {
    emscripten::function("getHeapState", &get_heap_state);
    emscripten::function("getLog", &get_log);
    emscripten::function("createInt", &create_int);
    emscripten::function("createPair", &create_pair);
    emscripten::function("createString", &create_string);
    emscripten::function("createArray", &create_array);
    emscripten::function("setArrayElement", &set_array_element);
    emscripten::function("addRoot", &add_root_by_id);
    emscripten::function("removeRoot", &remove_root_by_id);
    emscripten::function("runGC", &run_gc);
    emscripten::function("resetHeap", &reset_heap);
}

#else
int main() {
    heap_init();

    // Demo: create some objects
    Object* a = gc_alloc_int(42);
    Object* b = gc_alloc_int(100);
    Object* c = gc_alloc_pair(a, b);
    gc_add_root(c);

    Object* d = gc_alloc_int(999); // not rooted, will be swept

    gc_collect();

    printf("\nHeap state after GC:\n");
    for (int i = 0; i < heap.num_objects; i++) {
        Object* obj = &heap.objects[i];
        printf("  [%d] type=%d alive=%d color=%d\n",
               obj->id, obj->type, obj->alive, obj->color);
    }

    return 0;
}
#endif
