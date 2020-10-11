#ifndef FILE_H_
#define FILE_H_

#include "page.h"

#include <string>

class FileManager final
{
 public:
    static FileManager& get();

    ~FileManager();

    [[nodiscard]] bool open(const std::string& filename);
    void close();

    [[nodiscard]] bool is_open() const;

    header_page_t* header() const;
    [[nodiscard]] bool update_header();

    [[nodiscard]] bool file_alloc_page(pagenum_t& pagenum);
    [[nodiscard]] bool file_free_page(pagenum_t pagenum);
    [[nodiscard]] bool file_read_page(pagenum_t pagenum, page_t* dest);
    [[nodiscard]] bool file_write_page(pagenum_t pagenum, const page_t* src);

 private:
    [[nodiscard]] bool read(size_t size, size_t offset, void* value);
    [[nodiscard]] bool write(size_t size, size_t offset, const void* value);

 private:
    int file_handle_{ -1 };
    header_page_t* header_{ nullptr };
};

// Allocate an on-disk page from the free page list
pagenum_t file_alloc_page();

// Free an on-disk page to the free page list
void file_free_page(pagenum_t pagenum);

// Read an on-disk page into the in-memory page structure(dest)
void file_read_page(pagenum_t pagenum, page_t* dest);

// Write an in-memory page(src) to the on-disk page
void file_write_page(pagenum_t pagenum, const page_t* src);

#endif  // FILE_H_
