/*
 * 数据库内核课设 - 重构版
 * 核心数据结构：链表（主存储） + AVL树（索引）
 * 功能：新建表、增删改查、保存/加载JSON
 * 检索：支持最大最小值、包含字符串、比较运算（AVL树 + 线性遍历对比）
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h> 
#include "cJSON.h" 
#include <time.h>

/*==================== 高精度计时器 ====================*/

// 高精度计时结构
typedef struct {
    LARGE_INTEGER start;
    LARGE_INTEGER end;
    LARGE_INTEGER freq;
} HighResTimer;

// 初始化计时器
static void timerStart(HighResTimer* t) {
    QueryPerformanceFrequency(&t->freq);//获取每秒技术次数，胡须通过计算返回实际时间
    QueryPerformanceCounter(&t->start);
}

// 结束计时，返回微秒
static double timerEndMicro(HighResTimer* t) {
    QueryPerformanceCounter(&t->end);
    return (double)(t->end.QuadPart - t->start.QuadPart) * 1000000.0 / t->freq.QuadPart;//实际时间（微秒） = (结束计数 - 开始计数) × 1,000,000 / 频率
}

// 结束计时，返回毫秒
static double timerEndMs(HighResTimer* t) {
    return timerEndMicro(t) / 1000.0;
}

/*==================== 基础数据结构定义 ====================*/

/* 1. Column - 列定义结构体
 * 描述：表的列（字段）定义，包含列名和数据类型
 * 
 * 成员：
 *   - name: 列名（动态分配的字符串指针）
 *   - type: 数据类型 (1=整数int, 2=字符串string)
 * 
 * 内存管理：name 需要动态分配和释放
 */
typedef struct {
    char* name;        // 列名指针，指向动态分配的字符串
    int type;          // 列的数据类型标识符
} Column;

/* 2. Cell - 单元格结构体
 * 描述：表中单个单元格的数据存储，使用联合体节省空间
 * 
 * 成员：
 *   - type: 数据类型 (与Column的type对应)
 *   - data: 联合体，根据type只使用其中一个字段
 *       - int_val: 整数值
 *       - str_val: 字符串指针（动态分配）
 * 
 * 设计思路：使用union联合体，同一内存空间存储不同类型，同时在输入是作为验证
 * 内存效率：比两个独立字段节省空间（只占用较大字段的大小）
 */
typedef struct {
    int type;          // 标识当前单元格存储的数据类型
    union {            // 联合体：整数和字符串共享同一内存空间
        int int_val;   // 如果type=1，使用此字段
        char* str_val; // 如果type=2，使用此字段（需动态分配）
    } data;
} Cell;

/* 3. RecordNode - 记录节点（链表节点）（行）
 * 描述：单链表节点，存储一行数据
 * 
 * 成员：
 *   - cells: 指向单元格数组的指针（数组大小 = 列数）
 *   - next: 指向下一个记录节点的指针
 * 
 * 数据结构：单链表
 * 时间复杂度：
 *   - 尾插入：O(1) （通过Table的tail指针）
 *   - 遍历：O(n)
 *   - 删除中间节点：O(n)
 */
typedef struct RecordNode {
    Cell* cells;               // 指向Cell数组，存储该行所有列的数据
    struct RecordNode* next;   // 指向下一行记录的指针（链表）
} RecordNode;

/*4. Table - 表结构体
 * 描述：完整的数据表，包含表头定义和数据记录
 * 
 * 成员：
 *   - numColumns: 列数
 *   - columns: 列定义数组指针
 *   - head: 链表头指针（指向第一条记录）
 *   - tail: 链表尾指针（指向最后一条记录，用于O(1)尾插入）
 *   - rowCount: 当前记录总数
 * 
 * 核心数据结构：单链表（带尾指针优化）
 * 设计优势：
 *   - 动态增长，不需要预分配空间
 *   - 尾指针使插入操作达到O(1)
 * 设计权衡：
 *   - 不支持随机访问（必须从头遍历）
 *   - 删除中间元素需要O(n)时间
 */
typedef struct {
    int numColumns;      // 表的列数
    Column* columns;     // 列定义数组（大小为numColumns）
    RecordNode* head;    // 链表头指针，指向第一条记录（NULL表示空表）
    RecordNode* tail;    // 链表尾指针，指向最后一条记录（用于快速尾插）
    int rowCount;        // 当前表中的记录总数
} Table;

/*5. AVLNode - AVL平衡二叉搜索树节点
 * 描述：平衡二叉树索引结构，支持高效查找、范围查询
 * 
 * 成员：
 *   - intKey/strKey: 索引键（根据keyType选择使用）
 *   - keyType: 键的类型 (1=整数, 2=字符串)
 *   - record: 指向对应的RecordNode（不拥有所有权）
 *   - left/right: 左右子树指针
 *   - height: 当前节点的高度（用于平衡计算）
 * 
 * 核心算法：AVL树（自平衡二叉搜索树）
 * 平衡条件：|height(left) - height(right)| <= 1
 * 时间复杂度：
 *   - 查找：O(log n)
 *   - 插入：O(log n)
 *   - 删除：O(log n)
 * 
 * 设计说明：
 *   - record指针不拥有所有权，由Table的链表管理
 *   - height从叶子节点开始计数，空节点高度为0
 */
typedef struct AVLNode {
    //两种键类型支持对不同类型列建立索引
    int intKey;              // 整数类型的索引键
    char* strKey;            // 字符串类型的索引键（动态分配）
    int keyType;             // 1=使用intKey, 2=使用strKey
    //只存指针，不拷贝数据
    RecordNode* record;      // 指向实际数据记录（不拥有所有权）
    struct AVLNode* left;    // 左子树指针（键值 < 当前节点）
    struct AVLNode* right;   // 右子树指针（键值 > 当前节点）
    int height;              // 节点高度（用于计算平衡因子）
} AVLNode;

/*6. SearchResult - 搜索结果集，返回多条记录
 * 描述：动态数组，存储查询结果（支持多条记录）
 * 
 * 成员：
 *   - records: 指向RecordNode指针数组
 *   - rowNums: 对应的行号数组（从1开始编号）
 *   - count: 当前结果数量
 *   - capacity: 数组容量（自动扩容）
 * 
 * 数据结构：动态数组
 * 扩容策略：容量不足时 capacity *= 2
 * 初始容量：16
 * 
 * 内存管理：
 *   - records数组需要释放
 *   - rowNums数组需要释放
 *   - 但records指向的RecordNode由Table管理，不应释放
 */
typedef struct {
    RecordNode** records;  // 记录指针数组（动态分配）
    int* rowNums;          // 行号数组（与records一一对应）
    int count;             // 当前存储的结果数量
    int capacity;          // 数组容量（大于等于count）
} SearchResult;

/*==================== 前向声明 ====================*/
static void deepCopyCells(Cell* dest, Cell* src, int numColumns);
static void freeCells(Cell* cells, int numColumns);
RecordNode* addRecord(Table* table, Cell* cells);

/*==================== 表操作函数 ====================*/

/*createTable - 创建新表
 * 
 * 参数：
 *   @numColumns: 列数
 *   @columns: 列定义数组（函数会深拷贝，调用者可在之后释放）
 * 
 * 返回值：新创建的Table指针
 * 
 * 算法：
 *   1. 分配Table结构体内存
 *   2. 深拷贝列定义（包括列名字符串）
 *   3. 初始化链表为空（head=NULL, tail=NULL）
 *   4. 初始化行数为0
 * 
 * 内存管理：
 *   - 使用 _strdup 深拷贝列名，避免悬空指针
 *   - 返回的Table需要用 freeTable 释放
 * 
 * 时间复杂度：O(numColumns)
 * 
 * 
 * 算法：CreateTable(numColumns, columns[])━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

 输入：numColumns（整数），columns[]（列定义数组）
 输出：table（表指针）

 1. 内存分配阶段
   1.1 table ← ALLOCATE(Table)
   1.2 table.numColumns ← numColumns
   1.3 table.columns ← ALLOCATE_ARRAY(Column, numColumns)

 2. 数据拷贝阶段
   FOR i ← 0 TO numColumns - 1 DO
       2.1 newName ← DUPLICATE_STRING(columns[i].name)
       2.2 table.columns[i].name ← newName
       2.3 table.columns[i].type ← columns[i].type
   END FOR

 3. 初始化阶段
   3.1 table.head ← NULL
   3.2 table.tail ← NULL
   3.3 table.rowCount ← 0

 4. RETURN table


     时间复杂度：O(n × m)
      • n = numColumns（列数）
      • m = 平均列名长度
     空间复杂度：O(n × m)
      • 表结构体 + 列数组 + 所有列名字符串

 */
Table* createTable(int numColumns, Column* columns) {
    // 分配表结构体内存
    Table* table = (Table*)malloc(sizeof(Table));
    table->numColumns = numColumns;
    
    // 分配列定义数组
    table->columns = (Column*)malloc(numColumns * sizeof(Column));
    
    // 深拷贝每一列的定义
    for (int i = 0; i < numColumns; i++) {
        // 重要：深拷贝列名字符串，避免外部修改影响
        table->columns[i].name = _strdup(columns[i].name);
        table->columns[i].type = columns[i].type;
    }
    
    // 初始化空链表
    table->head = NULL;  // 头指针为空
    table->tail = NULL;  // 尾指针为空
    table->rowCount = 0; // 记录数为0
    
    return table;
}

/*freeTable - 释放表及其所有数据
 * 
 * 参数：
 *   @table: 要释放的表指针
 * 
 * 算法：
 *   1. 遍历链表，释放每个记录节点
 *   2. 对每个节点，先释放单元格中的字符串
 *   3. 释放列定义中的列名字符串
 *   4. 最后释放表结构体本身
 * 
 * 内存管理：
 *   - 必须按照依赖关系逆序释放（先释放内部，后释放外部）
 *   - 防止内存泄漏和重复释放
 * 
 * 时间复杂度：O(rowCount * numColumns)
 */
void freeTable(Table* table) {
    if (!table) return;  // 空指针检查
    
    // 遍历链表，释放所有记录节点
    RecordNode* current = table->head;
    while (current) {
        RecordNode* next = current->next;  // 保存下一个节点指针
        
        // 释放当前节点的单元格数据（包括字符串）
        freeCells(current->cells, table->numColumns);
        free(current->cells);  // 释放单元格数组
        free(current);         // 释放节点本身
        
        current = next;  // 移动到下一个节点
    }
    
    // 释放列定义中的列名字符串
    for (int i = 0; i < table->numColumns; i++) {
        free(table->columns[i].name);  // 释放 _strdup 分配的字符串
    }
    
    // 释放列定义数组和表结构体
    free(table->columns);
    free(table);
}

/*deepCopyCells - 深拷贝单元格数组
 * 
 * 参数：
 *   @dest: 目标单元格数组
 *   @src: 源单元格数组
 *   @numColumns: 列数（数组大小）
 * 
 * 算法：
 *   - 对每个单元格：
 *     - 如果是整数：直接复制值
 *     - 如果是字符串：使用 _strdup 深拷贝字符串
 * 
 * 为什么需要深拷贝：
 *   - 避免多个Cell指向同一字符串
 *   - 修改或删除一个Cell不会影响其他Cell
 * 
 * 时间复杂度：O(numColumns)
 */
static void deepCopyCells(Cell* dest, Cell* src, int numColumns) {
    for (int i = 0; i < numColumns; i++) {
        dest[i].type = src[i].type;
        
        if (src[i].type == 1) {
            // 整数类型：直接复制值
            dest[i].data.int_val = src[i].data.int_val;
        } else {
            // 字符串类型：深拷贝字符串
            const char* s = src[i].data.str_val ? src[i].data.str_val : "";
            dest[i].data.str_val = _strdup(s);  // 分配新内存并复制
        }
    }
}

/*freeCells - 释放单元格数组中的动态内存
 * 
 * 参数：
 *   @cells: 单元格数组
 *   @numColumns: 列数
 * 
 * 算法：
 *   遍历每个单元格，如果是字符串类型，释放字符串内存
 * 
 * 注意：
 *   - 只释放单元格内部的字符串，不释放cells数组本身
 *   - cells数组由调用者负责释放
 * 
 * 时间复杂度：O(numColumns)
 */
static void freeCells(Cell* cells, int numColumns) {
    if (!cells) return;  // 空指针检查
    
    for (int i = 0; i < numColumns; i++) {
        // 如果是字符串类型，释放动态分配的字符串
        if (cells[i].type != 1 && cells[i].data.str_val) {
            free(cells[i].data.str_val);
            cells[i].data.str_val = NULL;  // 防止悬空指针
        }
    }
}

/*addRecord - 添加新记录到表尾
 * 
 * 参数：
 *   @table: 目标表
 *   @cells: 新记录的单元格数组
 * 
 * 返回值：新创建的RecordNode指针，失败返回NULL
 * 
 * 算法：链表尾插法
 *   1. 验证单元格类型与表定义是否匹配
 *   2. 创建新节点并深拷贝单元格数据
 *   3. 如果链表为空，head和tail都指向新节点
 *   4. 否则，将新节点链接到tail后，更新tail指针
 * 
 * 时间复杂度：O(numColumns) - 因为有tail指针，不需要遍历链表
 * 空间复杂度：O(numColumns) - 深拷贝单元格数据
 * 
 * 优势：O(1)时间插入（通过维护tail指针）
 */
RecordNode* addRecord(Table* table, Cell* cells) {
    if (!table || !cells) return NULL;  // 参数校验

    // 类型验证：确保每列的类型与表定义匹配
    for (int i = 0; i < table->numColumns; i++) {
        if (cells[i].type != table->columns[i].type) {
            printf("Error: Column %d type mismatch!\n", i + 1);
            return NULL;
        }
    }

    // 分配新节点
    RecordNode* newNode = (RecordNode*)malloc(sizeof(RecordNode));
    if (!newNode) return NULL;
    
    // 分配单元格数组
    newNode->cells = (Cell*)malloc(table->numColumns * sizeof(Cell));
    if (!newNode->cells) { 
        free(newNode); 
        return NULL; 
    }
    
    // 深拷贝单元格数据（避免共享字符串指针）
    deepCopyCells(newNode->cells, cells, table->numColumns);
    newNode->next = NULL;  // 作为尾节点，next为NULL

    // 链表插入逻辑
    if (table->head == NULL) {
        // 情况1：空链表，head和tail都指向新节点
        table->head = newNode;
        table->tail = newNode;
    } else {
        // 情况2：非空链表，将新节点链接到tail后
        table->tail->next = newNode;
        table->tail = newNode;  // 更新tail指针
    }
    
    table->rowCount++;  // 行数加1
    return newNode;
}

/*deleteRecordByRowNum - 按行号删除记录
 * 
 * 参数：
 *   @table: 目标表
 *   @rowNum: 行号（从1开始）
 * 
 * 返回值：成功返回1，失败返回0
 * 
 * 算法：单链表删除
 *   1. 遍历链表找到第rowNum个节点
 *   2. 更新前驱节点的next指针跳过当前节点
 *   3. 处理特殊情况（删除头节点、尾节点）
 *   4. 释放被删除节点的内存
 * 
 * 时间复杂度：O(rowNum) - 需要遍历到目标位置
 * 
 * 关键指针操作：
 *   - prev: 当前节点的前驱节点
 *   - current: 当前遍历的节点
 *   - 删除时需要同步更新head/tail指针
 */
int deleteRecordByRowNum(Table* table, int rowNum) {
    // 参数校验：表不能为空，行号必须在有效范围内
    if (!table || rowNum < 1 || rowNum > table->rowCount) return 0;
    
    RecordNode* prev = NULL;      // 前驱节点指针
    RecordNode* current = table->head;  // 当前节点指针
    int idx = 1;  // 当前行号（从1开始）
    
    // 遍历链表找到第rowNum行
    while (current && idx < rowNum) {
        prev = current;          // 保存前驱节点
        current = current->next; // 移动到下一个节点
        idx++;
    }
    if (!current) return 0;  // 未找到目标节点

    // 更新链表结构
    if (prev) {
        // 情况1：删除的不是头节点，前驱节点跳过当前节点
        prev->next = current->next;
    } else {
        // 情况2：删除的是头节点，head指针后移
        table->head = current->next;
    }
    
    // 如果删除的是尾节点，更新tail指针
    if (table->tail == current) {
        table->tail = prev;  // prev可能为NULL（删除唯一节点）
    }

    // 释放被删除节点的内存
    freeCells(current->cells, table->numColumns);  // 释放单元格中的字符串
    free(current->cells);  // 释放单元格数组
    free(current);         // 释放节点本身
    table->rowCount--;     // 行数减1
    return 1;
}

/* updateRecordByRowNum - 按行号修改记录
 * 
 * 参数：
 *   @table: 目标表
 *   @rowNum: 行号（从1开始）
 *   @newCells: 新的单元格数据
 * 
 * 返回值：成功返回1，失败返回0
 * 
 * 算法：
 *   1. 验证新单元格类型与表定义匹配
 *   2. 遍历链表找到第rowNum个节点
 *   3. 释放旧单元格数据
 *   4. 深拷贝新单元格数据到节点
 * 
 * 时间复杂度：O(rowNum + numColumns)
 * 
 * 注意：不改变链表结构，只更新节点内容
 */
int updateRecordByRowNum(Table* table, int rowNum, Cell* newCells) {
    // 参数校验
    if (!table || !newCells || rowNum < 1 || rowNum > table->rowCount) return 0;
    
    // 类型校验：逐列确保新数据类型与表定义匹配
    for (int i = 0; i < table->numColumns; i++) {
        if (newCells[i].type != table->columns[i].type) {
            printf("Error: Column %d type mismatch!\n", i + 1);
            return 0;
        }
    }

    // 遍历链表找到目标节点
    RecordNode* current = table->head;
    int idx = 1;
    while (current && idx < rowNum) {
        current = current->next;
        idx++;
    }
    if (!current) return 0;  // 未找到目标节点

    // 更新单元格数据
    freeCells(current->cells, table->numColumns);  // 释放旧数据
    deepCopyCells(current->cells, newCells, table->numColumns);  // 拷贝新数据
    return 1;
}

// 获取指定行号的记录
RecordNode* getRecordByRowNum(Table* table, int rowNum) {
    if (!table || rowNum < 1 || rowNum > table->rowCount) return NULL;
    RecordNode* cur = table->head;
    int idx = 1;
    while (cur && idx < rowNum) {
        cur = cur->next;
        idx++;
    }
    return cur;
}

/*==================== JSON保存/加载 ====================*/

//导出保存为json
void saveTableToJson(Table* table, const char* filename) {
    //创建 JSON 根对象
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "numColumns", table->numColumns);
    
    //保存列定义
    cJSON* columnsArray = cJSON_CreateArray();
    //循环处理每一列
    for (int i = 0; i < table->numColumns; i++) {
        cJSON* col = cJSON_CreateObject();
        cJSON_AddStringToObject(col, "name", table->columns[i].name);
        cJSON_AddNumberToObject(col, "type", table->columns[i].type);
        cJSON_AddItemToArray(columnsArray, col);
    }
    cJSON_AddItemToObject(root, "columns", columnsArray);//将列数组添加到根对象
    
    // 保存记录数据
    cJSON* recordsArray = cJSON_CreateArray();//创建记录数组
   //遍历链表
    RecordNode* current = table->head;
    while (current) {
        cJSON* record = cJSON_CreateObject();//为每条记录创建对象
        // 遍历当前记录的所有列
        for (int i = 0; i < table->numColumns; i++) {
            if (table->columns[i].type == 1) {
                cJSON_AddNumberToObject(record, table->columns[i].name, current->cells[i].data.int_val);
            } 
            else {
                cJSON_AddStringToObject(record, table->columns[i].name, current->cells[i].data.str_val);
            }
        }
        cJSON_AddItemToArray(recordsArray, record);
        //将记录数组添加到根对象
        current = current->next;
    }
    cJSON_AddItemToObject(root, "records", recordsArray);
    
    //将 JSON 对象转换为格式化字符串
    char* jsonString = cJSON_Print(root);
    //写入文件
    FILE* file = fopen(filename, "w");
    if (file) {
        fprintf(file, "%s", jsonString);
        fclose(file);
    }
    //内存清理
    cJSON_Delete(root);
    free(jsonString);
}

//从json加载表格
Table* loadTableFromJson(const char* filename) {
    // 只读模式
    FILE* file = fopen(filename, "r");
    if (!file) return NULL;
    
    // 获取文件大小
    fseek(file, 0, SEEK_END); // 移动到文件末尾
    long size = ftell(file); // 获取当前位置（文件大小）
    fseek(file, 0, SEEK_SET);// 回到文件开头
    
    //读取文件内容
    char* jsonStr = (char*)malloc(size + 1);// 分配内存（+1 是为了 '\0'）
    fread(jsonStr, 1, size, file);// 读取整个文件
    jsonStr[size] = '\0';// 添加字符串结束符
    fclose(file);// 关闭文件
    
    //解析列定义
    cJSON* root = cJSON_Parse(jsonStr);// 解析 JSON 字符串为对象
    free(jsonStr);// 释放字符串内存（已经解析完了）
    if (!root) return NULL; // 解析失败
    
    int numColumns = cJSON_GetObjectItemCaseSensitive(root, "numColumns")->valueint;// 获取列数
    cJSON* columnsArray = cJSON_GetObjectItemCaseSensitive(root, "columns"); // 获取列数组
    
    //解析每一列定义
    Column* columns = (Column*)malloc(numColumns * sizeof(Column));
    for (int i = 0; i < numColumns; i++) {
        cJSON* col = cJSON_GetArrayItem(columnsArray, i);//获取第 i 个列对象
        columns[i].name = _strdup(cJSON_GetObjectItemCaseSensitive(col, "name")->valuestring);//  读取列名并复制
        columns[i].type = cJSON_GetObjectItemCaseSensitive(col, "type")->valueint;//  读取列类型
    }
    
    //创建表
    Table* table = createTable(numColumns, columns);
    // 释放临时的列定义数组
    for (int i = 0; i < numColumns; i++) free(columns[i].name);
    free(columns);
    
    //获取记录数组和数量
    cJSON* recordsArray = cJSON_GetObjectItemCaseSensitive(root, "records");
    int count = cJSON_GetArraySize(recordsArray);//有多少条记录
    for (int i = 0; i < count; i++) {//第635行：遍历每一条记录
        cJSON* record = cJSON_GetArrayItem(recordsArray, i);//获取第i条记录（JSON对象）
        Cell* cells = (Cell*)malloc(numColumns * sizeof(Cell));    // 第637行：为这一行分配单元格数组内存
       //第639行：根据列名从JSON记录中获取对应的值
        cJSON* value = cJSON_GetObjectItemCaseSensitive(record, table->columns[j].name);
        for (int j = 0; j < numColumns; j++) {
            cJSON* value = cJSON_GetObjectItemCaseSensitive(record, table->columns[j].name);
            cells[j].type = table->columns[j].type;
            if (table->columns[j].type == 1) {
                cells[j].data.int_val = value->valueint;
            } else {
                cells[j].data.str_val = _strdup(value->valuestring);
            }
        }
        addRecord(table, cells);
        freeCells(cells, numColumns);
        free(cells);
    }
    
    cJSON_Delete(root);
    return table;
}

/*==================== AVL树操作 ====================*/
/*AVL树（Adelson-Velsky and Landis Tree）是一种自平衡二叉搜索树
 * 
 * 核心特性：
 *   1. 左子树所有键值 < 根节点键值 < 右子树所有键值
 *   2. 任意节点的左右子树高度差（平衡因子）≤ 1
 *   3. 自动维护平衡，保证查找/插入/删除都是 O(log n)
 * 
 * 平衡因子 = 左子树高度 - 右子树高度
 *   - 取值范围：{-1, 0, 1}
 *   - 超出此范围需要旋转调整
 */

/*getHeight - 获取节点高度
 * 
 * 参数：@node: AVL树节点
 * 返回值：节点高度（空节点返回0）
 * 
 * 说明：
 *   - 叶子节点高度为1
 *   - 空节点高度定义为0
 *   - height存储在节点中，不需要递归计算
 * 
 * 时间复杂度：O(1)
 */
int getHeight(AVLNode* node) {
    return node ? node->height : 0;
}

/* maxInt - 返回两个整数的最大值辅助高度计算
 */
int maxInt(int a, int b) {
    return a > b ? a : b;
}

/*updateHeight - 更新节点高度
 * 
 * 参数：@node: AVL树节点
 * 
 * 算法：
 *   height = 1 + max(左子树高度, 右子树高度)
 * 
 * 调用时机：
 *   - 每次旋转后
 *   - 每次插入/删除后
 * 
 * 时间复杂度：O(1)
 */
void updateHeight(AVLNode* node) {
    if (node) {
        // 节点高度 = 1 + 左右子树中较高者的高度
        node->height = 1 + maxInt(getHeight(node->left), getHeight(node->right));
    }
}

/* getBalance - 计算平衡因子
 * 
 * 参数：@node: AVL树节点
 * 返回值：平衡因子（左子树高度 - 右子树高度）
 * 
 * 平衡因子含义：
 *   -1: 右子树比左子树高1（右偏）
 *    0: 左右子树等高（完全平衡）
 *    1: 左子树比右子树高1（左偏）
 *   >1: 左子树过高，需要右旋调整
 *   <-1: 右子树过高，需要左旋调整
 * 
 * 时间复杂度：O(1)
 */
int getBalance(AVLNode* node) {
    return node ? getHeight(node->left) - getHeight(node->right) : 0;
}

/* rotateRight - 右旋操作
 * 
 * 参数：@y: 旋转前的根节点
 * 返回值：旋转后的新根节点
 * 
 * 应用场景：左子树过高（平衡因子 > 1）
 * 
 * 旋转过程：
 *       y                    x
 *      / \                  / \
 *     x   T3    ==右旋==>   T1  y
 *    / \                      / \
 *   T1  T2                   T2  T3
 * 
 * 关键步骤：
 *   1. x = y的左子节点
 *   2. T2 = x的右子树（需要重新挂接）
 *   3. x的右子树指向y
 *   4. y的左子树指向T2
 *   5. 更新y和x的高度（先y后x，因为x依赖y）
 * 
 * 时间复杂度：O(1) - 只改变指针
 */
AVLNode* rotateRight(AVLNode* y) {
    AVLNode* x = y->left;      // x将成为新根
    AVLNode* T2 = x->right;    // T2将成为y的左子树
    
    // 执行旋转
    x->right = y;              // y成为x的右子树
    y->left = T2;              // T2成为y的左子树
    
    // 更新高度（必须先更新y，因为x的高度依赖y）
    updateHeight(y);
    updateHeight(x);
    
    return x;  // 返回新根节点
}

/* rotateLeft - 左旋操作
 * 
 * 参数：@x: 旋转前的根节点
 * 返回值：旋转后的新根节点
 * 
 * 应用场景：右子树过高（平衡因子 < -1）
 * 
 * 旋转过程：
 *     x                        y
 *    / \                      / \
 *   T1  y      ==左旋==>     x   T3
 *      / \                  / \
 *     T2  T3               T1  T2
 * 
 * 关键步骤：
 *   1. y = x的右子节点
 *   2. T2 = y的左子树（需要重新挂接）
 *   3. y的左子树指向x
 *   4. x的右子树指向T2
 *   5. 更新x和y的高度（先x后y）
 * 
 * 时间复杂度：O(1)
 */
AVLNode* rotateLeft(AVLNode* x) {
    AVLNode* y = x->right;     // y将成为新根
    AVLNode* T2 = y->left;     // T2将成为x的右子树
    
    // 执行旋转
    y->left = x;               // x成为y的左子树
    x->right = T2;             // T2成为x的右子树
    // 更新高度（必须先更新x，因为y的高度依赖x）
    updateHeight(x);
    updateHeight(y);
    
    return y;  // 返回新根节点
}

/* insertAVLInt - 插入整数键AVL节点
 * 
 * 参数：
 *   @node: 当前子树的根节点
 *   @key: 整数键值
 *   @record: 指向数据记录的指针
 * 
 * 返回值：插入后子树的新根节点
 * 
 * 算法：递归插入 + 自平衡调整
 *   1. 递归阶段：按二叉搜索树规则插入
 *      - key < 当前节点：插入左子树
 *      - key > 当前节点：插入右子树
 *      - key = 当前节点：不插入（去重）
 *   2. 回溯阶段：更新高度并检查平衡
 *      - 计算平衡因子
 *      - 根据失衡类型执行旋转
 * 
 * 四种失衡情况：
 *   1. LL（左左）：balance > 1 且新键在左子树的左侧 → 右旋
 *   2. RR（右右）：balance < -1 且新键在右子树的右侧 → 左旋
 *   3. LR（左右）：balance > 1 且新键在左子树的右侧 → 左旋左子树，右旋根
 *   4. RL（右左）：balance < -1 且新键在右子树的左侧 → 右旋右子树，左旋根
 * 
 * 时间复杂度：O(log n)
 * 空间复杂度：O(log n) - 递归调用栈
 */
AVLNode* insertAVLInt(AVLNode* node, int key, RecordNode* record) {
    // 基础情况：找到插入位置，创建新节点
    if (!node) {
        AVLNode* newNode = (AVLNode*)malloc(sizeof(AVLNode));
        newNode->intKey = key;
        newNode->strKey = NULL;
        newNode->keyType = 1;           // 整数类型
        newNode->record = record;       // 指向实际数据
        newNode->left = newNode->right = NULL;
        newNode->height = 1;            // 叶子节点高度为1
        return newNode;
    }

    // 递归插入（二叉搜索树规则）
    if (key < node->intKey) {
        // 键值小于当前节点，插入左子树
        node->left = insertAVLInt(node->left, key, record);
    } else if (key > node->intKey) {
        // 键值大于当前节点，插入右子树
        node->right = insertAVLInt(node->right, key, record);
    } else {
        // 键值相等，不插入重复键
        return node;
    }

    // 更新当前节点高度
    updateHeight(node);
    
    // 计算平衡因子
    int balance = getBalance(node);

    // 根据平衡因子判断失衡类型并旋转
    
    // 情况1：LL失衡（左子树的左侧插入导致左偏）
    if (balance > 1 && key < node->left->intKey) 
        return rotateRight(node);
    
    // 情况2：RR失衡（右子树的右侧插入导致右偏）
    if (balance < -1 && key > node->right->intKey) 
        return rotateLeft(node);
    
    // 情况3：LR失衡（左子树的右侧插入）
    // 需要双旋：先左旋左子树，再右旋根节点
    if (balance > 1 && key > node->left->intKey) {
        node->left = rotateLeft(node->left);   // 先左旋
        return rotateRight(node);               // 后右旋
    }
    
    // 情况4：RL失衡（右子树的左侧插入）
    // 需要双旋：先右旋右子树，再左旋根节点
    if (balance < -1 && key < node->right->intKey) {
        node->right = rotateRight(node->right); // 先右旋
        return rotateLeft(node);                // 后左旋
    }
    
    // 平衡，返回当前节点
    return node;
}

// 插入AVL节点（字符串键）
AVLNode* insertAVLStr(AVLNode* node, const char* key, RecordNode* record) {
    //递归插入
    //创建新节点
    if (!node) {
        AVLNode* newNode = (AVLNode*)malloc(sizeof(AVLNode));
        newNode->intKey = 0; // 整数键不使用，设为0
        newNode->strKey = _strdup(key);// 复制字符串键
        newNode->keyType = 2;// 标记为字符串类型
        newNode->record = record;// 标记为字符串类型
        newNode->left = newNode->right = NULL;// 叶子节点
        newNode->height = 1;// 初始高度为1
        return newNode;
    }
    
    //递归查找插入位置
    int cmp = strcmp(key, node->strKey);
    if (cmp < 0) { // 插入左子树
        node->left = insertAVLStr(node->left, key, record);
    } else if (cmp > 0) {// 插入右子树
        node->right = insertAVLStr(node->right, key, record);
    } else {
        return node;
    }

    updateHeight(node); // 更新当前节点高度
    int balance = getBalance(node); // 计算平衡因子

    // LL失衡,右旋
    if (balance > 1 && strcmp(key, node->left->strKey) < 0) return rotateRight(node);
    //RR失衡,左旋
    if (balance < -1 && strcmp(key, node->right->strKey) > 0) return rotateLeft(node);
    //左子树太高，但新键插入在左子树的右侧:先左旋后右旋
    if (balance > 1 && strcmp(key, node->left->strKey) > 0) {
        node->left = rotateLeft(node->left);
        return rotateRight(node);
    }
    //右子树太高，但新键插入在右子树的左侧：先右旋后左旋
    if (balance < -1 && strcmp(key, node->right->strKey) < 0) {
        node->right = rotateRight(node->right);
        return rotateLeft(node);
    }
    return node;
}

// 释放AVL树
void freeAVL(AVLNode* root) {
    if (root) {
        freeAVL(root->left);
        freeAVL(root->right);
        if (root->strKey) free(root->strKey);
        free(root);
    }
}

// 为指定列构建AVL索引
AVLNode* buildAVLIndex(Table* table, int colIndex) {
    //表指针不为空,列索引不能超出范围,列索引不能超出范围
    if (!table || colIndex < 0 || colIndex >= table->numColumns) return NULL;
    
    //初始化
    AVLNode* root = NULL;// AVL树根节点，初始为空
    RecordNode* cur = table->head; // 从链表头开始遍历
    
    //根据列类型构建索引
    if (table->columns[colIndex].type == 1) {//整数型
        while (cur) {
            //提取该记录在 colIndex 列的整数值：cur->cells[colIndex].data.int_val
            root = insertAVLInt(root, cur->cells[colIndex].data.int_val, cur);
            cur = cur->next;
        }
    } else {//字符串
        while (cur) {
            root = insertAVLStr(root, cur->cells[colIndex].data.str_val, cur);
            cur = cur->next;
        }
    }
    return root;
}

/*==================== 检索结果管理 ====================*/


// 创建结果集
SearchResult* createSearchResult() {
    SearchResult* sr = (SearchResult*)malloc(sizeof(SearchResult));//// 1. 分配容器结构体本身
    sr->capacity = 16;
    sr->count = 0;
    //// 4. 分配内部的动态数组
    sr->records = (RecordNode**)malloc(sr->capacity * sizeof(RecordNode*));
    sr->rowNums = (int*)malloc(sr->capacity * sizeof(int));
    return sr;// 返回新创建的容器指针
}

//添加结果 
void addToResultWithRowNum(SearchResult* sr, RecordNode* rec, int rowNum) {
    if (sr->count >= sr->capacity) {//// 1. 检查容量是否足够
        sr->capacity *= 2;
        sr->records = (RecordNode**)realloc(sr->records, sr->capacity * sizeof(RecordNode*));
        sr->rowNums = (int*)realloc(sr->rowNums, sr->capacity * sizeof(int));
    }
    // 4. 将新记录和行号添加到数组末尾
    sr->records[sr->count] = rec;
    sr->rowNums[sr->count] = rowNum;
    // 5. 更新计数器
    sr->count++;
}

// 兼容旧调用（行号设为0表示未知）提供一个更简单的接口，用于添加记录
void addToResult(SearchResult* sr, RecordNode* rec) {
    addToResultWithRowNum(sr, rec, 0);
}

//释放内存
void freeSearchResult(SearchResult* sr) {
    if (sr) {
        free(sr->records);
        free(sr->rowNums);
        free(sr);
    }
}

/*==================== 检索函数 ====================*/
//—————————————————————————————————最大最小查找————————————————————————————————————

// 线性遍历：查找最大值（返回记录和行号）
RecordNode* linearFindMax(Table* table, int colIndex, int* outRowNum) {
    if (!table || !table->head || table->columns[colIndex].type != 1) return NULL;
    //  初始化变量
    RecordNode* maxNode = table->head;
    RecordNode* cur = table->head->next;
    int maxRowNum = 1;
    int rowNum = 2;
    
    // 遍历链表，直到 cur 为 NULL
    while (cur) {
        if (cur->cells[colIndex].data.int_val > maxNode->cells[colIndex].data.int_val) {
            maxNode = cur;
            maxRowNum = rowNum;
        }
        cur = cur->next;
        rowNum++;
    }
    if (outRowNum) *outRowNum = maxRowNum;// 如果输出参数指针不为空，则将找到的行号写入
    return maxNode;
}

// 线性遍历：查找最小值（返回记录和行号）
RecordNode* linearFindMin(Table* table, int colIndex, int* outRowNum) {
    if (!table || !table->head || table->columns[colIndex].type != 1) return NULL;
    
    RecordNode* minNode = table->head;
    RecordNode* cur = table->head->next;
    int minRowNum = 1;
    int rowNum = 2;
    
    while (cur) {
        if (cur->cells[colIndex].data.int_val < minNode->cells[colIndex].data.int_val) {
            minNode = cur;
            minRowNum = rowNum;
        }
        cur = cur->next;
        rowNum++;
    }
    if (outRowNum) *outRowNum = minRowNum;
    return minNode;
}

/*avlFindMax - 查找AVL树中的最大值，不断向右
 * 
 * 参数：@root: AVL树根节点
 * 返回值：包含最大键值的节点指针
 * 
 * 算法原理：
 *   BST性质：最大值必定在最右侧的叶子节点
 *   从根节点开始，一直沿着右子树向下，直到没有右子节点
 * 
 * 示例：
 *        50
 *       /  \
 *      30   70
 *          /  \
 *         60   90  ← 最大值
 * 
 * 时间复杂度：O(log n) - 树高
 * 空间复杂度：O(1) - 迭代实现
 */
AVLNode* avlFindMax(AVLNode* root) {
    if (!root) return NULL;
    // 不断向右走，直到没有右子节点
    while (root->right) 
        root = root->right;
    return root;
}

/*avlFindMin - 查找AVL树中的最小值，不断向左
 * 
 * 参数：@root: AVL树根节点
 * 返回值：包含最小键值的节点指针
 * 
 * 算法原理：
 *   BST性质：最小值必定在最左侧的叶子节点
 *   从根节点开始，一直沿着左子树向下，直到没有左子节点
 * 
 * 示例：
 *        50
 *       /  \
 *      30   70
 *     /
 *    10  ← 最小值
 * 
 * 时间复杂度：O(log n) - 树高
 * 空间复杂度：O(1) - 迭代实现
 */
AVLNode* avlFindMin(AVLNode* root) {
    if (!root) return NULL;
    // 不断向左走，直到没有左子节点
    while (root->left) 
        root = root->left;
    return root;
}

//————————————————————————————————————TOPn查找————————————————————————————————————————————
/*SortItem - 排序TOPN辅助结构
 * 用于Top N查找时临时存储记录信息
 */
typedef struct {
    RecordNode* record;  // 记录指针
    int rowNum;          // 行号
    int value;           // 排序依据的值
} SortItem;

/*cmpDescending - qsort基础降序比较函数
 * 用于qsort，查找最大的前N项
 * 
 * 返回值：
 *   正数：b > a （b应排在a前面）
 *   0：   b = a
 *   负数：b < a
 */
static int cmpDescending(const void* a, const void* b) {
    return ((SortItem*)b)->value - ((SortItem*)a)->value;
}

/* cmpAscending - qsort基础升序比较函数
 * 用于qsort，查找最小的前N项
 * 
 * 返回值：
 *   负数：a < b （a应排在b前面）
 *   0：   a = b
 *   正数：a > b
 */
static int cmpAscending(const void* a, const void* b) {
    return ((SortItem*)a)->value - ((SortItem*)b)->value;
}

/* linearFindTopN - 线性查找最大的前N项
 * 
 * 参数：
 *   @table: 数据表
 *   @colIndex: 列索引
 *   @n: 需要返回的记录数量
 * 
 * 返回值：包含前N大记录的SearchResult
 * 
 * 算法：
 *   1. 遍历链表，收集所有记录到数组
 *   2. 使用qsort按值降序排序
 *   3. 取前N个元素
 * 
 * 时间复杂度：
 *   - 收集：O(n)
 *   - 排序：O(n log n) - qsort使用快速排序
 *   - 取值：O(N)
 *   总计：O(n log n)
 * 
 * 空间复杂度：O(n) - 临时数组
 * 
 * 应用场景：找分数最高的前10名学生、薪资最高的前20名员工
 */
SearchResult* linearFindTopN(Table* table, int colIndex, int n) {
    // 参数校验
    if (!table || !table->head || table->columns[colIndex].type != 1 || n <= 0) {
        return createSearchResult();
    }
    
    // 收集所有记录到临时数组
    int total = table->rowCount;
    SortItem* items = (SortItem*)malloc(total * sizeof(SortItem));
    RecordNode* cur = table->head;
    int idx = 0;//  临时数组的索引，从0开始
    int rowNum = 1;//当前遍历的行号，从1开始
    
    // 遍历链表，填充数组
    while (cur) {
        items[idx].record = cur;// 将当前记录节点的指针存入数组
        items[idx].rowNum = rowNum;// 将当前行号存入数组
        items[idx].value = cur->cells[colIndex].data.int_val;  // 提取排序键
        idx++;
        cur = cur->next;
        rowNum++;
    }
    
    // 降序排序（最大值在前）
    qsort(items, total, sizeof(SortItem), cmpDescending);
    
    // 取前n个（如果总数不足n，则取全部）
    SearchResult* sr = createSearchResult();
    int count = (n < total) ? n : total;// 计算实际要取的记录数（不能超过总数）
    for (int i = 0; i < count; i++) {
        addToResultWithRowNum(sr, items[i].record, items[i].rowNum);
    }
    
    free(items);  // 释放临时数组
    return sr;
}

// 线性遍历：查找最小的前n项
SearchResult* linearFindBottomN(Table* table, int colIndex, int n) {
    if (!table || !table->head || table->columns[colIndex].type != 1 || n <= 0) {
        return createSearchResult();
    }
    
    // 收集所有记录
    int total = table->rowCount;
    SortItem* items = (SortItem*)malloc(total * sizeof(SortItem));
    RecordNode* cur = table->head;
    int idx = 0;
    int rowNum = 1;
    while (cur) {
        items[idx].record = cur;
        items[idx].rowNum = rowNum;
        items[idx].value = cur->cells[colIndex].data.int_val;
        idx++;
        cur = cur->next;
        rowNum++;
    }
    
    // 升序排序
    qsort(items, total, sizeof(SortItem), cmpAscending);
    
    // 取前n个
    SearchResult* sr = createSearchResult();
    int count = (n < total) ? n : total;
    for (int i = 0; i < count; i++) {
        addToResultWithRowNum(sr, items[i].record, items[i].rowNum);
    }
    
    free(items);
    return sr;
}

// AVL树：逆中序遍历收集最大的n个（右-根-左）,核心递归函数
static void avlCollectTopN(AVLNode* node, SearchResult* sr, int n, int* collected) {
    if (!node || *collected >= n) return;
    //优先访问右子树
    avlCollectTopN(node->right, sr, n, collected);
    if (*collected < n) {
        addToResult(sr, node->record);  // AVL遍历时行号未知，设为0
        (*collected)++;
    }
    avlCollectTopN(node->left, sr, n, collected);
}

//AVL树 Top N 查找的入口函数
SearchResult* avlFindTopN(AVLNode* root, int n) {
    SearchResult* sr = createSearchResult();
    int collected = 0;
    avlCollectTopN(root, sr, n, &collected);//启动核心的 Top N 收集过程
    return sr;
}

// AVL树：中序遍历收集最小的n个（左-根-右）
static void avlCollectBottomN(AVLNode* node, SearchResult* sr, int n, int* collected) {
    if (!node || *collected >= n) return;
    avlCollectBottomN(node->left, sr, n, collected);
    if (*collected < n) {
        addToResult(sr, node->record);
        (*collected)++;
    }
    avlCollectBottomN(node->right, sr, n, collected);
}

SearchResult* avlFindBottomN(AVLNode* root, int n) {
    SearchResult* sr = createSearchResult();
    int collected = 0;
    avlCollectBottomN(root, sr, n, &collected);
    return sr;
}

// 线性遍历：等值查找（整数）- 带行号
SearchResult* linearFindEqual(Table* table, int colIndex, int value) {
    SearchResult* sr = createSearchResult();
    RecordNode* cur = table->head;
    int rowNum = 1;
    while (cur) {
        if (cur->cells[colIndex].type == 1 && cur->cells[colIndex].data.int_val == value) {
            addToResultWithRowNum(sr, cur, rowNum);
        }
        cur = cur->next;
        rowNum++;
    }
    return sr;
}

// 线性遍历：大于等于 - 带行号
SearchResult* linearFindGE(Table* table, int colIndex, int value) {
    SearchResult* sr = createSearchResult();
    RecordNode* cur = table->head;
    int rowNum = 1;
    while (cur) {
        if (cur->cells[colIndex].type == 1 && cur->cells[colIndex].data.int_val >= value) {
            addToResultWithRowNum(sr, cur, rowNum);
        }
        cur = cur->next;
        rowNum++;
    }
    return sr;
}

// 线性遍历：小于等于 - 带行号
SearchResult* linearFindLE(Table* table, int colIndex, int value) {
    SearchResult* sr = createSearchResult();
    RecordNode* cur = table->head;
    int rowNum = 1;
    while (cur) {
        if (cur->cells[colIndex].type == 1 && cur->cells[colIndex].data.int_val <= value) {
            addToResultWithRowNum(sr, cur, rowNum);
        }
        cur = cur->next;
        rowNum++;
    }
    return sr;
}

/*linearFindContains - 线性查找包含子字符串的记录
 * 
 * 参数：
 *   @table: 数据表
 *   @colIndex: 列索引
 *   @substr: 子字符串
 * 
 * 返回值：包含所有匹配记录的SearchResult
 * 
 * 算法：
 *   遍历链表，使用strstr检查每个字符串是否包含子串
 * 
 * 时间复杂度：O(n * m) 
 *   - n: 记录数
 *   - m: 字符串平均长度（strstr的复杂度）
 * 
 * 应用场景：模糊搜索，如查找姓名包含"李"的所有学生
 */
SearchResult* linearFindContains(Table* table, int colIndex, const char* substr) {
    SearchResult* sr = createSearchResult();
    RecordNode* cur = table->head;
    int rowNum = 1;
    
    // 遍历链表
    while (cur) {
        // 检查类型和指针有效性
        if (cur->cells[colIndex].type == 2 && cur->cells[colIndex].data.str_val) {
            // strstr: 查找子串，找到返回位置指针，未找到返回NULL
            if (strstr(cur->cells[colIndex].data.str_val, substr)) {
                addToResultWithRowNum(sr, cur, rowNum);
            }
        }
        cur = cur->next;  // 移动到下一个节点
        rowNum++;
    }
    return sr;
}

/*linearFindStrEqual - 线性查找字符串精确匹配
 * 
 * 参数：
 *   @table: 数据表
 *   @colIndex: 列索引
 *   @value: 目标字符串
 * 
 * 返回值：包含所有匹配记录的SearchResult
 * 
 * 算法：
 *   遍历链表，使用strcmp检查每个字符串是否完全相等
 * 
 * 时间复杂度：O(n * m)
 *   - n: 记录数
 *   - m: 字符串平均长度（strcmp的复杂度）
 * 
 * 与Contains的区别：
 *   - Equal: "张三" 只匹配 "张三"
 *   - Contains: "张三" 可以匹配 "张三丰"、"小张三"等
 */
SearchResult* linearFindStrEqual(Table* table, int colIndex, const char* value) {
    SearchResult* sr = createSearchResult();
    RecordNode* cur = table->head;
    int rowNum = 1;
    
    // 遍历链表
    while (cur) {
        // 检查类型和指针有效性
        if (cur->cells[colIndex].type == 2 && cur->cells[colIndex].data.str_val) {
            // strcmp: 字符串比较，相等返回0
            if (strcmp(cur->cells[colIndex].data.str_val, value) == 0) {
                addToResultWithRowNum(sr, cur, rowNum);
            }
        }
        cur = cur->next;
        rowNum++;
    }
    return sr;
}

/* avlFindGEHelper - AVL树范围查找辅助函数（>=）
 * 
 * 参数：
 *   @node: 当前节点
 *   @value: 阈值
 *   @sr: 结果集（用于收集匹配的节点）
 * 
 * 算法：改进的中序遍历
 *   - 如果 node->key >= value:
 *       左子树可能有满足条件的节点 → 递归左子树
 *       当前节点满足条件 → 加入结果
 *       右子树全部满足条件 → 递归右子树
 *   - 如果 node->key < value:
 *       左子树全部 < value → 剪枝，不递归
 *       只递归右子树
 * 
 * 优化：利用BST性质剪枝，避免遍历不可能满足条件的子树
 * 
 * 时间复杂度：O(log n + k)
 *   - log n: 找到第一个满足条件的节点
 *   - k: 满足条件的节点数量
 */
void avlFindGEHelper(AVLNode* node, int value, SearchResult* sr) {
    if (!node) return;  // 递归基：空节点
    
    if (node->intKey >= value) {
        // 当前节点 >= value，左子树可能有满足条件的
        avlFindGEHelper(node->left, value, sr);  // 递归左子树
        addToResult(sr, node->record);           // 加入当前节点
        avlFindGEHelper(node->right, value, sr); // 递归右子树
    } else {
        // 当前节点 < value，左子树肯定全部 < value（剪枝）
        avlFindGEHelper(node->right, value, sr); // 只递归右子树
    }
}

/*avlFindGE - AVL树范围查找接口（>=）
 * 
 * 参数：
 *   @root: AVL树根节点
 *   @value: 阈值
 * 
 * 返回值：包含所有 key >= value 的记录的SearchResult
 * 
 * 时间复杂度：O(log n + k)，优于线性查找的O(n)
 */
SearchResult* avlFindGE(AVLNode* root, int value) {
    SearchResult* sr = createSearchResult();
    avlFindGEHelper(root, value, sr);
    return sr;
}

// AVL树：范围查找 <= value
void avlFindLEHelper(AVLNode* node, int value, SearchResult* sr) {
    if (!node) return;
    if (node->intKey <= value) {
        avlFindLEHelper(node->left, value, sr);
        addToResult(sr, node->record);
        avlFindLEHelper(node->right, value, sr);
    } else {
        avlFindLEHelper(node->left, value, sr);
    }
}

///*avlFindGE - AVL树范围查找接口（<=）
SearchResult* avlFindLE(AVLNode* root, int value) {
    SearchResult* sr = createSearchResult();
    avlFindLEHelper(root, value, sr);
    return sr;
}

// AVL树：等值查找
AVLNode* avlFindEqual(AVLNode* root, int value) {
    while (root) {
        if (value < root->intKey) root = root->left;
        else if (value > root->intKey) root = root->right;
        else return root;
    }
    return NULL;
}

/*==================== 工具函数 ====================*/

// 控制台输入转 UTF-8（用于处理 Windows 控制台输入）
// Windows PowerShell/cmd 实际上使用系统代码页 (通常是 GBK/936)，即使设置了 65001
static void consoleInputToUtf8(char* dest, const char* src, int destSize) {
    // 检查是否有非 ASCII 字符
    int hasNonAscii = 0;
    const unsigned char* p = (const unsigned char*)src;
    while (*p) {
        if (*p >= 0x80) {
            hasNonAscii = 1;
            break;
        }
        p++;
    }
    
    // 如果是纯 ASCII，直接复制
    if (!hasNonAscii) {
        strncpy(dest, src, destSize - 1);
        dest[destSize - 1] = '\0';
        return;
    }
    
    // 对于非 ASCII 输入，强制使用系统代码页 (CP_ACP，通常是 GBK/936) 进行转换
    // 因为 Windows 传统控制台不真正支持 UTF-8 输入
    
    // 步骤1: 系统代码页 (GBK) -> Unicode (wchar_t)
    int wlen = MultiByteToWideChar(CP_ACP, 0, src, -1, NULL, 0);
    if (wlen <= 0) {
        strncpy(dest, src, destSize - 1);
        dest[destSize - 1] = '\0';
        return;
    }
    
    wchar_t* wbuf = (wchar_t*)malloc(wlen * sizeof(wchar_t));
    if (!wbuf) {
        strncpy(dest, src, destSize - 1);
        dest[destSize - 1] = '\0';
        return;
    }
    
    MultiByteToWideChar(CP_ACP, 0, src, -1, wbuf, wlen);
    
    // 步骤2: Unicode -> UTF-8
    int utf8len = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, NULL, 0, NULL, NULL);
    if (utf8len <= 0 || utf8len > destSize) {
        free(wbuf);
        strncpy(dest, src, destSize - 1);
        dest[destSize - 1] = '\0';
        return;
    }
    
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, dest, destSize, NULL, NULL);
    free(wbuf);
}

static void readLine(char* buf, int size) {
    fflush(stdout);
    char tempBuf[512];
    if (fgets(tempBuf, sizeof(tempBuf), stdin)) {
        size_t len = strlen(tempBuf);
        if (len > 0 && tempBuf[len - 1] == '\n') tempBuf[len - 1] = '\0';
        len = strlen(tempBuf);
        if (len > 0 && tempBuf[len - 1] == '\r') tempBuf[len - 1] = '\0';
        
        // 将可能的 GBK 输入转换为 UTF-8
        consoleInputToUtf8(buf, tempBuf, size);
    } else if (size > 0) {
        buf[0] = '\0';
    }
}

static void waitEnter() {
    printf("Press Enter to continue...");
    fflush(stdout);
    int c;
    while ((c = getchar()) != '\n' && c != EOF) {}
}

// 打印表格（确保显示列名）
static void printTable(Table* table) {
    if (!table) {
        printf("[Info] No table loaded.\n");
        return;
    }
    
    printf("\n=== Table (Rows: %d, Columns: %d) ===\n", table->rowCount, table->numColumns);
    
    // 打印表头
    printf("| %-4s", "No.");
    for (int i = 0; i < table->numColumns; i++) {
        printf(" | %-14s", table->columns[i].name);
    }
    printf(" |\n");
    
    // 分隔线
    printf("|------");
    for (int i = 0; i < table->numColumns; i++) {
        printf("|----------------");
    }
    printf("|\n");
    
    // 打印记录
    RecordNode* cur = table->head;
    int idx = 1;
    while (cur) {
        printf("| %-4d", idx);
        for (int i = 0; i < table->numColumns; i++) {
            if (table->columns[i].type == 1) {
                printf(" | %-14d", cur->cells[i].data.int_val);
            } else {
                const char* s = cur->cells[i].data.str_val;
                printf(" | %-14s", s ? s : "(null)");
            }
        }
        printf(" |\n");
        cur = cur->next;
        idx++;
    }
    
    if (table->rowCount == 0) {
        printf("[Info] Table is empty.\n");
    }
}

// 打印单条记录
static void printRecord(Table* table, RecordNode* node) {
    if (!table || !node) return;
    printf("Record: ");
    for (int i = 0; i < table->numColumns; i++) {
        printf("%s=", table->columns[i].name);
        if (table->columns[i].type == 1) {
            printf("%d", node->cells[i].data.int_val);
        } else {
            printf("%s", node->cells[i].data.str_val);
        }
        if (i < table->numColumns - 1) printf(", ");
    }
    printf("\n");
}

// 打印检索结果（带行号和序号）
static void printSearchResults(Table* table, SearchResult* sr) {
    if (!sr || sr->count == 0) {
        printf("[Info] No results found.\n");
        return;
    }
    printf("Found %d record(s):\n", sr->count);
    for (int i = 0; i < sr->count && i < 50; i++) { // 最多显示50条
        printf("  [%d] (Row %d) ", i + 1, sr->rowNums[i]);
        printRecord(table, sr->records[i]);
    }
    if (sr->count > 50) {
        printf("  ... and %d more.\n", sr->count - 50);
    }
}

// 通用交互式检索函数（用于删除/修改前的筛选）
// 返回检索结果，调用者负责释放
static SearchResult* interactiveSearch(Table* table) {
    if (!table || table->rowCount == 0) {
        printf("Table is empty.\n");
        return NULL;
    }
    
    int ch;
    
    // 选择列
    printf("Select column to search:\n");
    for (int i = 0; i < table->numColumns; i++) {
        printf("  [%d] %s (%s)\n", i, table->columns[i].name,
               table->columns[i].type == 1 ? "int" : "string");
    }
    printf("Column index: ");
    fflush(stdout);
    int colIdx;
    if (scanf("%d", &colIdx) != 1 || colIdx < 0 || colIdx >= table->numColumns) {
        while ((ch = getchar()) != '\n' && ch != EOF) {}
        printf("Invalid column.\n");
        return NULL;
    }
    while ((ch = getchar()) != '\n' && ch != EOF) {}
    
    // 选择条件
    printf("Search condition:\n");
    if (table->columns[colIdx].type == 1) {
        printf("  1. Find MAX (single)\n");
        printf("  2. Find MIN (single)\n");
        printf("  3. Equal to value (=)\n");
        printf("  4. Greater or equal (>=)\n");
        printf("  5. Less or equal (<=)\n");
        printf("  7. Find TOP N (largest)\n");
        printf("  8. Find BOTTOM N (smallest)\n");
    } else {
        printf("  3. Equal to value (=)\n");
        printf("  6. Contains substring\n");
    }
    printf("Condition: ");
    fflush(stdout);
    int cond;
    if (scanf("%d", &cond) != 1) {
        while ((ch = getchar()) != '\n' && ch != EOF) {}
        return NULL;
    }
    while ((ch = getchar()) != '\n' && ch != EOF) {}
    
    SearchResult* sr = NULL;
    
    if (cond == 1 && table->columns[colIdx].type == 1) {
        // 最大值
        int rowNum = 0;
        RecordNode* rec = linearFindMax(table, colIdx, &rowNum);
        if (rec) {
            sr = createSearchResult();
            addToResultWithRowNum(sr, rec, rowNum);
        }
    } else if (cond == 2 && table->columns[colIdx].type == 1) {
        // 最小值
        int rowNum = 0;
        RecordNode* rec = linearFindMin(table, colIdx, &rowNum);
        if (rec) {
            sr = createSearchResult();
            addToResultWithRowNum(sr, rec, rowNum);
        }
    } else if (cond == 3 && table->columns[colIdx].type == 1) {
        // 整数等于
        printf("Enter value: ");
        fflush(stdout);
        int val;
        scanf("%d", &val);
        while ((ch = getchar()) != '\n' && ch != EOF) {}
        sr = linearFindEqual(table, colIdx, val);
    } else if (cond == 3 && table->columns[colIdx].type == 2) {
        // 字符串等于
        char buf[128];
        printf("Enter value: ");
        fflush(stdout);
        readLine(buf, sizeof(buf));
        sr = linearFindStrEqual(table, colIdx, buf);
    } else if (cond == 4 && table->columns[colIdx].type == 1) {
        // 大于等于
        printf("Enter value: ");
        fflush(stdout);
        int val;
        scanf("%d", &val);
        while ((ch = getchar()) != '\n' && ch != EOF) {}
        sr = linearFindGE(table, colIdx, val);
    } else if (cond == 5 && table->columns[colIdx].type == 1) {
        // 小于等于
        printf("Enter value: ");
        fflush(stdout);
        int val;
        scanf("%d", &val);
        while ((ch = getchar()) != '\n' && ch != EOF) {}
        sr = linearFindLE(table, colIdx, val);
    } else if (cond == 6 && table->columns[colIdx].type == 2) {
        // 包含字符串
        char buf[128];
        printf("Enter substring: ");
        fflush(stdout);
        readLine(buf, sizeof(buf));
        sr = linearFindContains(table, colIdx, buf);
    } else if (cond == 7 && table->columns[colIdx].type == 1) {
        // 最大前n项
        printf("Enter N (top N largest): ");
        fflush(stdout);
        int n;
        scanf("%d", &n);
        while ((ch = getchar()) != '\n' && ch != EOF) {}
        if (n > 0) {
            sr = linearFindTopN(table, colIdx, n);
        }
    } else if (cond == 8 && table->columns[colIdx].type == 1) {
        // 最小前n项
        printf("Enter N (bottom N smallest): ");
        fflush(stdout);
        int n;
        scanf("%d", &n);
        while ((ch = getchar()) != '\n' && ch != EOF) {}
        if (n > 0) {
            sr = linearFindBottomN(table, colIdx, n);
        }
    } else {
        printf("Invalid condition.\n");
        return NULL;
    }
    
    return sr;
}

/*==================== 主函数 ====================*/

int main() {
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
    
    Table* table = NULL;
    int running = 1;
    int autoDisplay = 1;

    while (running) {
        printf("\n========== MENU ==========\n");
        printf("1. Create Table\n");
        printf("2. Add Record\n");
        printf("3. Search Records\n");
        printf("4. Delete Record\n");
        printf("5. Modify Record\n");
        printf("6. Save to JSON\n");
        printf("7. Load from JSON\n");
        printf("8. Settings (Auto Display)\n");
        printf("0. Exit\n");
        printf("Choose: ");
        fflush(stdout);
        
        int choice = -1;
        if (scanf("%d", &choice) != 1) {
            int ch; while ((ch = getchar()) != '\n' && ch != EOF) {}
            continue;
        }
        int ch; while ((ch = getchar()) != '\n' && ch != EOF) {}

        switch (choice) {
        case 1: { // Create Table
            if (table) { freeTable(table); table = NULL; }
            
            printf("Number of columns: ");
            int n;
            if (scanf("%d", &n) != 1 || n <= 0) {
                while ((ch = getchar()) != '\n' && ch != EOF) {}
                printf("Invalid.\n");
                break;
            }
            while ((ch = getchar()) != '\n' && ch != EOF) {}
            
            Column* cols = (Column*)malloc(n * sizeof(Column));
            for (int i = 0; i < n; i++) {
                char nameBuf[64];
                printf("Column %d name: ", i);
                readLine(nameBuf, sizeof(nameBuf));
                cols[i].name = _strdup(nameBuf);
                printf("Column %d type (1=int, 2=string): ", i);
                int t = 0;
                scanf("%d", &t);
                while ((ch = getchar()) != '\n' && ch != EOF) {}
                cols[i].type = (t == 1) ? 1 : 2;
            }
            
            table = createTable(n, cols);
            for (int i = 0; i < n; i++) free(cols[i].name);
            free(cols);
            
            printf("Table created. Columns:\n");
            for (int i = 0; i < table->numColumns; i++) {
                printf("  [%d] %s (%s)\n", i, table->columns[i].name,
                       table->columns[i].type == 1 ? "int" : "string");
            }
            break;
        }
        
        case 2: { // Add Record
            if (!table) { printf("Create table first.\n"); break; }
            
            Cell* cells = (Cell*)malloc(table->numColumns * sizeof(Cell));
            for (int i = 0; i < table->numColumns; i++) {
                cells[i].type = table->columns[i].type;
                if (table->columns[i].type == 1) {
                    printf("Enter [%s] (int): ", table->columns[i].name);
                    fflush(stdout);
                    scanf("%d", &cells[i].data.int_val);
                    while ((ch = getchar()) != '\n' && ch != EOF) {}
                } else {
                    char buf[128];
                    printf("Enter [%s] (string): ", table->columns[i].name);
                    fflush(stdout);
                    readLine(buf, sizeof(buf));
                    cells[i].data.str_val = _strdup(buf);
                }
            }
            
            if (addRecord(table, cells)) {
                printf("Record added. Total rows: %d\n", table->rowCount);
            } else {
                printf("Failed to add record.\n");
            }
            freeCells(cells, table->numColumns);
            free(cells);
            break;
        }
        
        case 3: { // Search
            if (!table || table->rowCount == 0) {
                printf("Table is empty or not created.\n");
                break;
            }
            
            // 选择列
            printf("Select column to search:\n");
            for (int i = 0; i < table->numColumns; i++) {
                printf("  [%d] %s (%s)\n", i, table->columns[i].name,
                       table->columns[i].type == 1 ? "int" : "string");
            }
            printf("Column index: ");
            int colIdx;
            if (scanf("%d", &colIdx) != 1 || colIdx < 0 || colIdx >= table->numColumns) {
                while ((ch = getchar()) != '\n' && ch != EOF) {}
                printf("Invalid column.\n");
                break;
            }
            while ((ch = getchar()) != '\n' && ch != EOF) {}
            
            // 选择条件
            printf("Search condition:\n");
            if (table->columns[colIdx].type == 1) {
                printf("  1. Find MAX (single)\n");
                printf("  2. Find MIN (single)\n");
                printf("  3. Equal to value (=)\n");
                printf("  4. Greater or equal (>=)\n");
                printf("  5. Less or equal (<=)\n");
                printf("  7. Find TOP N (largest)\n");
                printf("  8. Find BOTTOM N (smallest)\n");
            } else {
                printf("  6. Contains substring\n");
            }
            printf("Condition: ");
            int cond;
            if (scanf("%d", &cond) != 1) {
                while ((ch = getchar()) != '\n' && ch != EOF) {}
                break;
            }
            while ((ch = getchar()) != '\n' && ch != EOF) {}
            
            HighResTimer timer;
            double linearTime, avlBuildTime, avlSearchTime;
            
            if (cond == 1 && table->columns[colIdx].type == 1) {
                // 最大值
                int rowNum1 = 0;
                timerStart(&timer);
                RecordNode* r1 = linearFindMax(table, colIdx, &rowNum1);
                linearTime = timerEndMicro(&timer);
                
                // AVL建树
                timerStart(&timer);
                AVLNode* avlRoot = buildAVLIndex(table, colIdx);
                avlBuildTime = timerEndMicro(&timer);
                
                // AVL查找
                timerStart(&timer);
                AVLNode* r2 = avlFindMax(avlRoot);
                avlSearchTime = timerEndMicro(&timer);
                freeAVL(avlRoot);
                
                printf("\n--- Results ---\n");
                printf("Linear search: %.2f us (%.4f ms) - Row %d\n", linearTime, linearTime/1000.0, rowNum1);
                if (r1) printRecord(table, r1);
                printf("AVL build:     %.2f us (%.4f ms)\n", avlBuildTime, avlBuildTime/1000.0);
                printf("AVL search:    %.2f us (%.4f ms)\n", avlSearchTime, avlSearchTime/1000.0);
                printf("AVL total:     %.2f us (%.4f ms)\n", avlBuildTime + avlSearchTime, (avlBuildTime + avlSearchTime)/1000.0);
                if (r2) printRecord(table, r2->record);
                
            } else if (cond == 2 && table->columns[colIdx].type == 1) {
                // 最小值
                int rowNum1 = 0;
                timerStart(&timer);
                RecordNode* r1 = linearFindMin(table, colIdx, &rowNum1);
                linearTime = timerEndMicro(&timer);
                
                timerStart(&timer);
                AVLNode* avlRoot = buildAVLIndex(table, colIdx);
                avlBuildTime = timerEndMicro(&timer);
                
                timerStart(&timer);
                AVLNode* r2 = avlFindMin(avlRoot);
                avlSearchTime = timerEndMicro(&timer);
                freeAVL(avlRoot);
                
                printf("\n--- Results ---\n");
                printf("Linear search: %.2f us (%.4f ms) - Row %d\n", linearTime, linearTime/1000.0, rowNum1);
                if (r1) printRecord(table, r1);
                printf("AVL build:     %.2f us (%.4f ms)\n", avlBuildTime, avlBuildTime/1000.0);
                printf("AVL search:    %.2f us (%.4f ms)\n", avlSearchTime, avlSearchTime/1000.0);
                printf("AVL total:     %.2f us (%.4f ms)\n", avlBuildTime + avlSearchTime, (avlBuildTime + avlSearchTime)/1000.0);
                if (r2) printRecord(table, r2->record);
                
            } else if (cond == 3 && table->columns[colIdx].type == 1) {
                // 等于
                printf("Enter value: ");
                int val;
                scanf("%d", &val);
                while ((ch = getchar()) != '\n' && ch != EOF) {}
                
                timerStart(&timer);
                SearchResult* sr1 = linearFindEqual(table, colIdx, val);
                linearTime = timerEndMicro(&timer);
                
                timerStart(&timer);
                AVLNode* avlRoot = buildAVLIndex(table, colIdx);
                avlBuildTime = timerEndMicro(&timer);
                
                timerStart(&timer);
                AVLNode* r2 = avlFindEqual(avlRoot, val);
                avlSearchTime = timerEndMicro(&timer);
                
                printf("\n--- Results ---\n");
                printf("Linear search: %.2f us (%.4f ms), found %d\n", linearTime, linearTime/1000.0, sr1->count);
                printSearchResults(table, sr1);
                printf("AVL build:     %.2f us (%.4f ms)\n", avlBuildTime, avlBuildTime/1000.0);
                printf("AVL search:    %.2f us (%.4f ms), %s\n", avlSearchTime, avlSearchTime/1000.0, r2 ? "found" : "not found");
                printf("AVL total:     %.2f us (%.4f ms)\n", avlBuildTime + avlSearchTime, (avlBuildTime + avlSearchTime)/1000.0);
                if (r2) printRecord(table, r2->record);
                
                freeSearchResult(sr1);
                freeAVL(avlRoot);
                
            } else if (cond == 4 && table->columns[colIdx].type == 1) {
                // 大于等于
                printf("Enter value: ");
                int val;
                scanf("%d", &val);
                while ((ch = getchar()) != '\n' && ch != EOF) {}
                
                timerStart(&timer);
                SearchResult* sr1 = linearFindGE(table, colIdx, val);
                linearTime = timerEndMicro(&timer);
                
                timerStart(&timer);
                AVLNode* avlRoot = buildAVLIndex(table, colIdx);
                avlBuildTime = timerEndMicro(&timer);
                
                timerStart(&timer);
                SearchResult* sr2 = avlFindGE(avlRoot, val);
                avlSearchTime = timerEndMicro(&timer);
                
                printf("\n--- Results ---\n");
                printf("Linear search: %.2f us (%.4f ms), found %d\n", linearTime, linearTime/1000.0, sr1->count);
                printSearchResults(table, sr1);
                printf("AVL build:     %.2f us (%.4f ms)\n", avlBuildTime, avlBuildTime/1000.0);
                printf("AVL search:    %.2f us (%.4f ms), found %d\n", avlSearchTime, avlSearchTime/1000.0, sr2->count);
                printf("AVL total:     %.2f us (%.4f ms)\n", avlBuildTime + avlSearchTime, (avlBuildTime + avlSearchTime)/1000.0);
                
                freeSearchResult(sr1);
                freeSearchResult(sr2);
                freeAVL(avlRoot);
                
            } else if (cond == 5 && table->columns[colIdx].type == 1) {
                // 小于等于
                printf("Enter value: ");
                int val;
                scanf("%d", &val);
                while ((ch = getchar()) != '\n' && ch != EOF) {}
                
                timerStart(&timer);
                SearchResult* sr1 = linearFindLE(table, colIdx, val);
                linearTime = timerEndMicro(&timer);
                
                timerStart(&timer);
                AVLNode* avlRoot = buildAVLIndex(table, colIdx);
                avlBuildTime = timerEndMicro(&timer);
                
                timerStart(&timer);
                SearchResult* sr2 = avlFindLE(avlRoot, val);
                avlSearchTime = timerEndMicro(&timer);
                
                printf("\n--- Results ---\n");
                printf("Linear search: %.2f us (%.4f ms), found %d\n", linearTime, linearTime/1000.0, sr1->count);
                printSearchResults(table, sr1);
                printf("AVL build:     %.2f us (%.4f ms)\n", avlBuildTime, avlBuildTime/1000.0);
                printf("AVL search:    %.2f us (%.4f ms), found %d\n", avlSearchTime, avlSearchTime/1000.0, sr2->count);
                printf("AVL total:     %.2f us (%.4f ms)\n", avlBuildTime + avlSearchTime, (avlBuildTime + avlSearchTime)/1000.0);
                
                freeSearchResult(sr1);
                freeSearchResult(sr2);
                freeAVL(avlRoot);
                
            } else if (cond == 6 && table->columns[colIdx].type == 2) {
                // 包含字符串
                char substr[128];
                printf("Enter substring: ");
                readLine(substr, sizeof(substr));
                
                timerStart(&timer);
                SearchResult* sr1 = linearFindContains(table, colIdx, substr);
                linearTime = timerEndMicro(&timer);
                
                printf("\n--- Results ---\n");
                printf("Linear search: %.2f us (%.4f ms), found %d\n", linearTime, linearTime/1000.0, sr1->count);
                printSearchResults(table, sr1);
                printf("(AVL not applicable for substring search)\n");
                
                freeSearchResult(sr1);
                
            } else if (cond == 7 && table->columns[colIdx].type == 1) {
                // 最大前n项
                printf("Enter N (top N largest): ");
                int n;
                scanf("%d", &n);
                while ((ch = getchar()) != '\n' && ch != EOF) {}
                if (n <= 0) { printf("Invalid N.\n"); break; }
                
                timerStart(&timer);
                SearchResult* sr1 = linearFindTopN(table, colIdx, n);
                linearTime = timerEndMicro(&timer);
                
                timerStart(&timer);
                AVLNode* avlRoot = buildAVLIndex(table, colIdx);
                avlBuildTime = timerEndMicro(&timer);
                
                timerStart(&timer);
                SearchResult* sr2 = avlFindTopN(avlRoot, n);
                avlSearchTime = timerEndMicro(&timer);
                
                printf("\n--- Results (Top %d) ---\n", n);
                printf("Linear (with sort): %.2f us (%.4f ms), found %d\n", linearTime, linearTime/1000.0, sr1->count);
                printSearchResults(table, sr1);
                printf("AVL build:          %.2f us (%.4f ms)\n", avlBuildTime, avlBuildTime/1000.0);
                printf("AVL search:         %.2f us (%.4f ms), found %d\n", avlSearchTime, avlSearchTime/1000.0, sr2->count);
                printf("AVL total:          %.2f us (%.4f ms)\n", avlBuildTime + avlSearchTime, (avlBuildTime + avlSearchTime)/1000.0);
                
                freeSearchResult(sr1);
                freeSearchResult(sr2);
                freeAVL(avlRoot);
                
            } else if (cond == 8 && table->columns[colIdx].type == 1) {
                // 最小前n项
                printf("Enter N (bottom N smallest): ");
                int n;
                scanf("%d", &n);
                while ((ch = getchar()) != '\n' && ch != EOF) {}
                if (n <= 0) { printf("Invalid N.\n"); break; }
                
                timerStart(&timer);
                SearchResult* sr1 = linearFindBottomN(table, colIdx, n);
                linearTime = timerEndMicro(&timer);
                
                timerStart(&timer);
                AVLNode* avlRoot = buildAVLIndex(table, colIdx);
                avlBuildTime = timerEndMicro(&timer);
                
                timerStart(&timer);
                SearchResult* sr2 = avlFindBottomN(avlRoot, n);
                avlSearchTime = timerEndMicro(&timer);
                
                printf("\n--- Results (Bottom %d) ---\n", n);
                printf("Linear (with sort): %.2f us (%.4f ms), found %d\n", linearTime, linearTime/1000.0, sr1->count);
                printSearchResults(table, sr1);
                printf("AVL build:          %.2f us (%.4f ms)\n", avlBuildTime, avlBuildTime/1000.0);
                printf("AVL search:         %.2f us (%.4f ms), found %d\n", avlSearchTime, avlSearchTime/1000.0, sr2->count);
                printf("AVL total:          %.2f us (%.4f ms)\n", avlBuildTime + avlSearchTime, (avlBuildTime + avlSearchTime)/1000.0);
                
                freeSearchResult(sr1);
                freeSearchResult(sr2);
                freeAVL(avlRoot);
                
            } else {
                printf("Invalid condition for this column type.\n");
            }
            break;
        }
        
        case 4: { // Delete（先检索再删除）
            if (!table || table->rowCount == 0) {
                printf("Table is empty.\n");
                break;
            }
            
            printf("=== DELETE: First search for records ===\n");
            printf("1. Search by condition\n");
            printf("2. Enter row number directly\n");
            printf("Choose: ");
            fflush(stdout);
            int delMode;
            if (scanf("%d", &delMode) != 1) {
                while ((ch = getchar()) != '\n' && ch != EOF) {}
                break;
            }
            while ((ch = getchar()) != '\n' && ch != EOF) {}
            
            if (delMode == 1) {
                // 通过检索筛选
                SearchResult* sr = interactiveSearch(table);
                if (!sr || sr->count == 0) {
                    printf("No records found.\n");
                    if (sr) freeSearchResult(sr);
                    break;
                }
                
                printf("\n--- Search Results ---\n");
                printSearchResults(table, sr);
                
                printf("\nOptions:\n");
                printf("  Enter result number (1-%d) to delete that record\n", sr->count);
                printf("  Enter 0 to delete ALL found records\n");
                printf("  Enter -1 to cancel\n");
                printf("Your choice: ");
                fflush(stdout);
                int delChoice;
                if (scanf("%d", &delChoice) != 1) {
                    while ((ch = getchar()) != '\n' && ch != EOF) {}
                    freeSearchResult(sr);
                    break;
                }
                while ((ch = getchar()) != '\n' && ch != EOF) {}
                
                if (delChoice == -1) {
                    printf("Cancelled.\n");
                } else if (delChoice == 0) {
                    // 删除所有找到的记录（从后往前删，避免行号变化）
                    // 先收集所有行号并排序（降序）
                    int* rowsToDelete = (int*)malloc(sr->count * sizeof(int));
                    for (int i = 0; i < sr->count; i++) {
                        rowsToDelete[i] = sr->rowNums[i];
                    }
                    // 简单冒泡降序排序
                    for (int i = 0; i < sr->count - 1; i++) {
                        for (int j = 0; j < sr->count - 1 - i; j++) {
                            if (rowsToDelete[j] < rowsToDelete[j+1]) {
                                int tmp = rowsToDelete[j];
                                rowsToDelete[j] = rowsToDelete[j+1];
                                rowsToDelete[j+1] = tmp;
                            }
                        }
                    }
                    int deleted = 0;
                    for (int i = 0; i < sr->count; i++) {
                        if (deleteRecordByRowNum(table, rowsToDelete[i])) {
                            deleted++;
                        }
                    }
                    free(rowsToDelete);
                    printf("Deleted %d record(s). Remaining rows: %d\n", deleted, table->rowCount);
                } else if (delChoice >= 1 && delChoice <= sr->count) {
                    int rowNum = sr->rowNums[delChoice - 1];
                    if (deleteRecordByRowNum(table, rowNum)) {
                        printf("Deleted row %d. Remaining rows: %d\n", rowNum, table->rowCount);
                    } else {
                        printf("Delete failed.\n");
                    }
                } else {
                    printf("Invalid choice.\n");
                }
                freeSearchResult(sr);
            } else {
                // 直接输入行号
                printf("Enter row number to delete (1-%d): ", table->rowCount);
                fflush(stdout);
                int rowNum;
                if (scanf("%d", &rowNum) != 1) {
                    while ((ch = getchar()) != '\n' && ch != EOF) {}
                    break;
                }
                while ((ch = getchar()) != '\n' && ch != EOF) {}
                
                if (deleteRecordByRowNum(table, rowNum)) {
                    printf("Deleted. Remaining rows: %d\n", table->rowCount);
                } else {
                    printf("Delete failed.\n");
                }
            }
            break;
        }
        
        case 5: { // Modify（先检索再修改）
            if (!table || table->rowCount == 0) {
                printf("Table is empty.\n");
                break;
            }
            
            printf("=== MODIFY: First search for record ===\n");
            printf("1. Search by condition\n");
            printf("2. Enter row number directly\n");
            printf("Choose: ");
            fflush(stdout);
            int modMode;
            if (scanf("%d", &modMode) != 1) {
                while ((ch = getchar()) != '\n' && ch != EOF) {}
                break;
            }
            while ((ch = getchar()) != '\n' && ch != EOF) {}
            
            int targetRowNum = -1;
            
            if (modMode == 1) {
                // 通过检索筛选
                SearchResult* sr = interactiveSearch(table);
                if (!sr || sr->count == 0) {
                    printf("No records found.\n");
                    if (sr) freeSearchResult(sr);
                    break;
                }
                
                printf("\n--- Search Results ---\n");
                printSearchResults(table, sr);
                
                printf("\nEnter result number to modify (1-%d), or -1 to cancel: ", sr->count);
                fflush(stdout);
                int modChoice;
                if (scanf("%d", &modChoice) != 1) {
                    while ((ch = getchar()) != '\n' && ch != EOF) {}
                    freeSearchResult(sr);
                    break;
                }
                while ((ch = getchar()) != '\n' && ch != EOF) {}
                
                if (modChoice == -1) {
                    printf("Cancelled.\n");
                    freeSearchResult(sr);
                    break;
                } else if (modChoice >= 1 && modChoice <= sr->count) {
                    targetRowNum = sr->rowNums[modChoice - 1];
                } else {
                    printf("Invalid choice.\n");
                    freeSearchResult(sr);
                    break;
                }
                freeSearchResult(sr);
            } else {
                // 直接输入行号
                printf("Enter row number to modify (1-%d): ", table->rowCount);
                fflush(stdout);
                if (scanf("%d", &targetRowNum) != 1 || targetRowNum < 1 || targetRowNum > table->rowCount) {
                    while ((ch = getchar()) != '\n' && ch != EOF) {}
                    printf("Invalid row number.\n");
                    break;
                }
                while ((ch = getchar()) != '\n' && ch != EOF) {}
            }
            
            if (targetRowNum < 1) break;
            
            RecordNode* oldRec = getRecordByRowNum(table, targetRowNum);
            if (oldRec) {
                printf("\nCurrent record (Row %d):\n", targetRowNum);
                printRecord(table, oldRec);
            }
            
            printf("\nEnter new values:\n");
            Cell* cells = (Cell*)malloc(table->numColumns * sizeof(Cell));
            for (int i = 0; i < table->numColumns; i++) {
                cells[i].type = table->columns[i].type;
                if (table->columns[i].type == 1) {
                    printf("  [%s] (int): ", table->columns[i].name);
                    fflush(stdout);
                    scanf("%d", &cells[i].data.int_val);
                    while ((ch = getchar()) != '\n' && ch != EOF) {}
                } else {
                    char buf[128];
                    printf("  [%s] (string): ", table->columns[i].name);
                    fflush(stdout);
                    readLine(buf, sizeof(buf));
                    cells[i].data.str_val = _strdup(buf);
                }
            }
            
            if (updateRecordByRowNum(table, targetRowNum, cells)) {
                printf("Record updated.\n");
            } else {
                printf("Update failed.\n");
            }
            freeCells(cells, table->numColumns);
            free(cells);
            break;
        }
        
        case 6: { // Save
            if (!table) { printf("No table to save.\n"); break; }
            char fname[128];
            printf("Filename: ");
            readLine(fname, sizeof(fname));
            saveTableToJson(table, fname);
            printf("Saved to %s\n", fname);
            break;
        }
        
        case 7: { // Load
            char fname[128];
            printf("Filename: ");
            readLine(fname, sizeof(fname));
            Table* newTable = loadTableFromJson(fname);
            if (!newTable) {
                printf("Load failed.\n");
                break;
            }
            if (table) freeTable(table);
            table = newTable;
            printf("Loaded. Rows: %d, Columns: %d\n", table->rowCount, table->numColumns);
            for (int i = 0; i < table->numColumns; i++) {
                printf("  [%d] %s (%s)\n", i, table->columns[i].name,
                       table->columns[i].type == 1 ? "int" : "string");
            }
            break;
        }
        
        case 8: { // Settings
            printf("Auto display table: %s\n", autoDisplay ? "ON" : "OFF");
            printf("Enter 1=ON, 0=OFF: ");
            int v;
            if (scanf("%d", &v) == 1) {
                autoDisplay = (v != 0);
                printf("Set to: %s\n", autoDisplay ? "ON" : "OFF");
            }
            while ((ch = getchar()) != '\n' && ch != EOF) {}
            break;
        }
        
        case 0:
            running = 0;
            break;
            
        default:
            printf("Invalid option.\n");
        }
        
        if (choice != 0) {
            if (autoDisplay && table) {
                printTable(table);
            }
            waitEnter();
        }
    }

    if (table) freeTable(table);
    printf("Goodbye!\n");
    return 0;
}
