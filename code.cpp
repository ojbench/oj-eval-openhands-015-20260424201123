
#include <iostream>
#include <fstream>
#include <cstring>
#include <vector>
#include <algorithm>

using namespace std;

const int MAX_INDEX_LEN = 65;
const int BLOCK_SIZE = 4096;

struct Key {
    char index[MAX_INDEX_LEN];
    int value;

    Key() {
        memset(index, 0, sizeof(index));
        value = 0;
    }

    Key(const char* idx, int val) {
        memset(index, 0, sizeof(index));
        if (idx) strcpy(index, idx);
        value = val;
    }

    bool operator<(const Key& other) const {
        int cmp = strcmp(index, other.index);
        if (cmp != 0) return cmp < 0;
        return value < other.value;
    }

    bool operator==(const Key& other) const {
        return value == other.value && strcmp(index, other.index) == 0;
    }

    bool operator<=(const Key& other) const {
        return *this < other || *this == other;
    }

    bool operator>(const Key& other) const {
        return other < *this;
    }

    bool operator>=(const Key& other) const {
        return !(*this < other);
    }
};

struct NodeHeader {
    bool is_leaf;
    int count;
    long long parent;
    long long next; // Only for leaf nodes
};

// Use a slightly smaller number to be safe from alignment issues
const int MAX_KEYS_LEAF = 40;
const int MAX_KEYS_INTERNAL = 40;

// Using a struct with a buffer to ensure BLOCK_SIZE and avoid union issues
struct Node {
    char buffer[BLOCK_SIZE];

    Node() {
        memset(buffer, 0, BLOCK_SIZE);
    }

    struct Leaf {
        NodeHeader header;
        Key keys[MAX_KEYS_LEAF + 1];
    };

    struct Internal {
        NodeHeader header;
        Key keys[MAX_KEYS_INTERNAL + 1];
        long long children[MAX_KEYS_INTERNAL + 2];
    };

    Leaf& as_leaf() {
        return *reinterpret_cast<Leaf*>(buffer);
    }

    Internal& as_internal() {
        return *reinterpret_cast<Internal*>(buffer);
    }
};

struct FileHeader {
    long long root_offset;
    long long free_list_head;
    long long total_blocks;
};

class BPlusTree {
    string filename;
    fstream file;
    FileHeader header;

    void read_header() {
        file.seekg(0);
        file.read(reinterpret_cast<char*>(&header), sizeof(FileHeader));
    }

    void write_header() {
        file.seekp(0);
        file.write(reinterpret_cast<const char*>(&header), sizeof(FileHeader));
    }

    long long allocate_block() {
        long long offset;
        if (header.free_list_head != 0) {
            offset = header.free_list_head;
            file.seekg(offset);
            file.read(reinterpret_cast<char*>(&header.free_list_head), sizeof(long long));
        } else {
            offset = header.total_blocks * BLOCK_SIZE;
            header.total_blocks++;
        }
        write_header();
        return offset;
    }

    void free_block(long long offset) {
        file.seekp(offset);
        file.write(reinterpret_cast<const char*>(&header.free_list_head), sizeof(long long));
        header.free_list_head = offset;
        write_header();
    }

public:
    BPlusTree(string name) : filename(name) {
        file.open(filename, ios::in | ios::out | ios::binary);
        if (!file) {
            file.open(filename, ios::out | ios::binary);
            file.close();
            file.open(filename, ios::in | ios::out | ios::binary);
            header.root_offset = 0;
            header.free_list_head = 0;
            header.total_blocks = 1; // Block 0 is header
            write_header();
        } else {
            read_header();
        }
    }

    ~BPlusTree() {
        if (file.is_open()) {
            file.close();
        }
    }

    void insert(const char* index, int value) {
        Key key(index, value);
        if (header.root_offset == 0) {
            long long offset = allocate_block();
            Node node;
            node.as_leaf().header.is_leaf = true;
            node.as_leaf().header.count = 1;
            node.as_leaf().header.parent = 0;
            node.as_leaf().header.next = 0;
            node.as_leaf().keys[0] = key;
            file.seekp(offset);
            file.write(node.buffer, BLOCK_SIZE);
            header.root_offset = offset;
            write_header();
            return;
        }

        long long curr_offset = header.root_offset;
        while (true) {
            Node node;
            file.seekg(curr_offset);
            file.read(node.buffer, BLOCK_SIZE);
            if (node.as_internal().header.is_leaf) break;

            int i = 0;
            while (i < node.as_internal().header.count && key >= node.as_internal().keys[i]) {
                i++;
            }
            curr_offset = node.as_internal().children[i];
        }

        Node node;
        file.seekg(curr_offset);
        file.read(node.buffer, BLOCK_SIZE);

        int i = 0;
        while (i < node.as_leaf().header.count && node.as_leaf().keys[i] < key) {
            i++;
        }
        if (i < node.as_leaf().header.count && node.as_leaf().keys[i] == key) {
            return; // Duplicate key-value pair
        }

        for (int j = node.as_leaf().header.count; j > i; j--) {
            node.as_leaf().keys[j] = node.as_leaf().keys[j - 1];
        }
        node.as_leaf().keys[i] = key;
        node.as_leaf().header.count++;

        if (node.as_leaf().header.count <= MAX_KEYS_LEAF) {
            file.seekp(curr_offset);
            file.write(node.buffer, BLOCK_SIZE);
        } else {
            split_leaf(curr_offset, node);
        }
    }

    void split_leaf(long long offset, Node& node) {
        long long new_offset = allocate_block();
        Node new_node;
        new_node.as_leaf().header.is_leaf = true;
        new_node.as_leaf().header.parent = node.as_leaf().header.parent;
        new_node.as_leaf().header.next = node.as_leaf().header.next;
        node.as_leaf().header.next = new_offset;

        int mid = node.as_leaf().header.count / 2;
        new_node.as_leaf().header.count = node.as_leaf().header.count - mid;
        node.as_leaf().header.count = mid;

        for (int i = 0; i < new_node.as_leaf().header.count; i++) {
            new_node.as_leaf().keys[i] = node.as_leaf().keys[mid + i];
        }

        file.seekp(offset);
        file.write(node.buffer, BLOCK_SIZE);
        file.seekp(new_offset);
        file.write(new_node.buffer, BLOCK_SIZE);

        insert_into_parent(offset, new_node.as_leaf().keys[0], new_offset, node.as_leaf().header.parent);
    }

    void insert_into_parent(long long left_offset, Key key, long long right_offset, long long parent_offset) {
        if (parent_offset == 0) {
            long long new_root_offset = allocate_block();
            Node new_root;
            new_root.as_internal().header.is_leaf = false;
            new_root.as_internal().header.count = 1;
            new_root.as_internal().header.parent = 0;
            new_root.as_internal().keys[0] = key;
            new_root.as_internal().children[0] = left_offset;
            new_root.as_internal().children[1] = right_offset;
            file.seekp(new_root_offset);
            file.write(new_root.buffer, BLOCK_SIZE);
            header.root_offset = new_root_offset;
            write_header();

            update_parent(left_offset, new_root_offset);
            update_parent(right_offset, new_root_offset);
            return;
        }

        Node parent;
        file.seekg(parent_offset);
        file.read(parent.buffer, BLOCK_SIZE);

        int i = 0;
        while (i < parent.as_internal().header.count && parent.as_internal().keys[i] < key) {
            i++;
        }
        for (int j = parent.as_internal().header.count; j > i; j--) {
            parent.as_internal().keys[j] = parent.as_internal().keys[j - 1];
            parent.as_internal().children[j + 1] = parent.as_internal().children[j];
        }
        parent.as_internal().keys[i] = key;
        parent.as_internal().children[i + 1] = right_offset;
        parent.as_internal().header.count++;

        if (parent.as_internal().header.count <= MAX_KEYS_INTERNAL) {
            file.seekp(parent_offset);
            file.write(parent.buffer, BLOCK_SIZE);
            update_parent(right_offset, parent_offset);
        } else {
            split_internal(parent_offset, parent);
        }
    }

    void split_internal(long long offset, Node& node) {
        long long new_offset = allocate_block();
        Node new_node;
        new_node.as_internal().header.is_leaf = false;
        new_node.as_internal().header.parent = node.as_internal().header.parent;

        int mid = node.as_internal().header.count / 2;
        Key up_key = node.as_internal().keys[mid];

        new_node.as_internal().header.count = node.as_internal().header.count - mid - 1;
        node.as_internal().header.count = mid;

        for (int i = 0; i < new_node.as_internal().header.count; i++) {
            new_node.as_internal().keys[i] = node.as_internal().keys[mid + 1 + i];
        }
        for (int i = 0; i <= new_node.as_internal().header.count; i++) {
            new_node.as_internal().children[i] = node.as_internal().children[mid + 1 + i];
        }

        file.seekp(offset);
        file.write(node.buffer, BLOCK_SIZE);
        file.seekp(new_offset);
        file.write(new_node.buffer, BLOCK_SIZE);

        for (int i = 0; i <= new_node.as_internal().header.count; i++) {
            update_parent(new_node.as_internal().children[i], new_offset);
        }

        insert_into_parent(offset, up_key, new_offset, node.as_internal().header.parent);
    }

    void update_parent(long long offset, long long parent_offset) {
        Node node;
        file.seekg(offset);
        file.read(node.buffer, BLOCK_SIZE);
        node.as_internal().header.parent = parent_offset;
        file.seekp(offset);
        file.write(node.buffer, BLOCK_SIZE);
    }

    void find(const char* index) {
        if (header.root_offset == 0) {
            cout << "null" << endl;
            return;
        }

        Key search_key(index, -1);
        long long curr_offset = header.root_offset;
        while (true) {
            Node node;
            file.seekg(curr_offset);
            file.read(node.buffer, BLOCK_SIZE);
            if (node.as_internal().header.is_leaf) break;

            int i = 0;
            while (i < node.as_internal().header.count && search_key >= node.as_internal().keys[i]) {
                i++;
            }
            curr_offset = node.as_internal().children[i];
        }

        bool found = false;
        bool first = true;
        while (curr_offset != 0) {
            Node node;
            file.seekg(curr_offset);
            file.read(node.buffer, BLOCK_SIZE);
            for (int i = 0; i < node.as_leaf().header.count; i++) {
                int cmp = strcmp(node.as_leaf().keys[i].index, index);
                if (cmp == 0) {
                    if (!first) cout << " ";
                    cout << node.as_leaf().keys[i].value;
                    found = true;
                    first = false;
                } else if (cmp > 0) {
                    goto end_find;
                }
            }
            curr_offset = node.as_leaf().header.next;
        }

    end_find:
        if (!found) cout << "null";
        cout << endl;
    }

    void remove(const char* index, int value) {
        Key key(index, value);
        if (header.root_offset == 0) return;

        long long curr_offset = header.root_offset;
        while (true) {
            Node node;
            file.seekg(curr_offset);
            file.read(node.buffer, BLOCK_SIZE);
            if (node.as_internal().header.is_leaf) break;

            int i = 0;
            while (i < node.as_internal().header.count && key >= node.as_internal().keys[i]) {
                i++;
            }
            curr_offset = node.as_internal().children[i];
        }

        Node node;
        file.seekg(curr_offset);
        file.read(node.buffer, BLOCK_SIZE);

        int i = 0;
        while (i < node.as_leaf().header.count && node.as_leaf().keys[i] < key) {
            i++;
        }
        if (i == node.as_leaf().header.count || !(node.as_leaf().keys[i] == key)) {
            return; // Not found
        }

        for (int j = i; j < node.as_leaf().header.count - 1; j++) {
            node.as_leaf().keys[j] = node.as_leaf().keys[j + 1];
        }
        node.as_leaf().header.count--;
        
        file.seekp(curr_offset);
        file.write(node.buffer, BLOCK_SIZE);
    }
};

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    BPlusTree tree("data.db");

    int n;
    if (!(cin >> n)) return 0;

    while (n--) {
        string cmd;
        cin >> cmd;
        if (cmd == "insert") {
            char index[MAX_INDEX_LEN];
            int value;
            cin >> index >> value;
            tree.insert(index, value);
        } else if (cmd == "delete") {
            char index[MAX_INDEX_LEN];
            int value;
            cin >> index >> value;
            tree.remove(index, value);
        } else if (cmd == "find") {
            char index[MAX_INDEX_LEN];
            cin >> index;
            tree.find(index);
        }
    }

    return 0;
}
