#pragma once
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>

#include "pipe.h"
#include "buffer.h"
#include "mask_filter.h"


namespace perg
{

// stupid RAII perf measurement timer
class metric
{
public:
	metric()
	{
		started = std::chrono::steady_clock::now();
	}
	~metric()
	{
		auto stopped = std::chrono::steady_clock::now();
		std::cout << "elapsed " << (stopped - started).count() << std::endl;
	}
private:
	using time_t = std::chrono::time_point<std::chrono::steady_clock>;
	time_t started;
};

// writes all incoming messages into a binary buffer
class search_result : public sink<view>
{
public:
	search_result()
		: _limit(std::numeric_limits<int>::max())
		, _count(0)
		, _separator(0)
	{
	}

	void dump(stream& ss)
	{
		_buffer.dump(ss);
	}

	void limit(int num)
	{
		_limit = num;
	}
	
	void separate_by(char separator)
	{
		_separator = separator; 
	}

protected:
	virtual action process(view& v)
	{
		if (_count < _limit)
		{
			++_count;
			_buffer.copy(v.data(), v.size());

			if (_separator)
			{
				_buffer.copy(&_separator, 1);
			}

			return UNDECIDED;
		}

		return TERMINATE;
	}

private:
	buffer _buffer;
	int _limit;
	int _count;
	char _separator;
};

// reads file line by line, every read line translates to 
// a message passed downstream to a filter
// the file is being read in forward direction
class line_reader : public source<view>
{
public:
	line_reader()
		: _file_desc(-1)
		, _file_ptr(nullptr)
		, _file_end(nullptr)
		, _cur_ptr(nullptr)
		, _file_size(0)
	{
	}

	explicit line_reader(const char* filename)
		: _file_desc(-1)
		, _file_ptr(nullptr)
		, _file_end(nullptr)
		, _cur_ptr(nullptr)
		, _file_size(0)
	{
		open(filename);
	}

	void open(const char* filename)
	{
		assert(_file_desc == -1);
		int _file_desc = ::open(filename, O_RDONLY);
		if (_file_desc != -1)
		{
			struct stat sb;

			if (fstat(_file_desc, &sb) != -1)
			{
				_file_size = sb.st_size;
				_file_ptr = (char*)mmap(0, _file_size, PROT_READ, MAP_PRIVATE, _file_desc, 0);
				_file_end = _file_ptr + _file_size;
				_cur_ptr = _file_ptr;
			}
		}
	}

	~line_reader()
	{
		if (_file_ptr)
		{
			close(_file_desc);
			munmap(_file_ptr, _file_size);
		}
	}

protected:
	virtual action process(view& v)
	{
		if (_cur_ptr != _file_end)
		{
			char* nextLine = (char*)memchr(_cur_ptr, '\n', _file_end - _cur_ptr);
			size_t size = 0;
			if (nextLine)
			{
				// we do not count the newline char
				size = nextLine++ - _cur_ptr;
			}
			else
			{
				nextLine = _file_end;
				size = nextLine - _cur_ptr;
			}
			
			v.assign(_cur_ptr, size);
			_cur_ptr = nextLine;
			

			return PASS_DOWNSTREAM;
		}
		return TERMINATE;
	}
private:
	int _file_desc;
	char* _file_ptr;
	char* _file_end;
	char* _cur_ptr;
	size_t _file_size;

};

// need this of Mac OS
#ifndef __linux__
inline void* memrchr(const void* ptr, int ch, size_t len)
{
	if (!len) return nullptr;
	const unsigned char* p = (const unsigned char*)ptr + len - 1;
	while (*p != ch && p-- != ptr);
	return p >= ptr ? (void*)p : nullptr;
}
#endif

// reads file bacwards, the last line goes first
class reverse_line_reader : public source<view>
{
public:
	reverse_line_reader()
		: _file_desc(-1)
		, _file_ptr(nullptr)
		, _file_end(nullptr)
		, _cur_ptr(nullptr)
		, _file_size(0)
	{
	}

	explicit reverse_line_reader(const char* filename)
		: _file_desc(-1)
		, _file_ptr(nullptr)
		, _file_end(nullptr)
		, _cur_ptr(nullptr)
		, _file_size(0)
	{
		open(filename);
	}

	void open(const char* filename)
	{
		assert(_file_desc == -1);
		int _file_desc = ::open(filename, O_RDONLY);
		if (_file_desc != -1)
		{
			struct stat sb;

			if (fstat(_file_desc, &sb) != -1)
			{
				_file_size = sb.st_size;
				_file_ptr = (char*)mmap(0, _file_size, PROT_READ, MAP_SHARED, _file_desc, 0);
				_file_end = _file_ptr + _file_size;
				_cur_ptr = _file_end;
			}
		}
	}

	~reverse_line_reader()
	{
		if (_file_ptr)
		{
			close(_file_desc);
			munmap(_file_ptr, _file_size);
		}
	}

protected:
	virtual action process(view& v)
	{
		if (_cur_ptr != _file_ptr)
		{
			char* lineEnd = _cur_ptr[-1] == '\n' ? _cur_ptr - 1 : _cur_ptr;
			char* thisLine = (char*)memrchr(_file_ptr, '\n', lineEnd - _file_ptr);
			if (thisLine)
			{
				thisLine += 1;
			}
			else
			{
				thisLine = _file_ptr;
			}
			const size_t size = lineEnd - thisLine;
			
			v.assign(thisLine, size);
			_cur_ptr = thisLine;
			
			return PASS_DOWNSTREAM;
		}
		return TERMINATE;
	}
private:
	int _file_desc;
	char* _file_ptr;
	char* _file_end;
	char* _cur_ptr;
	size_t _file_size;

};

// reads stdin, and any other FILE* for that matter
class stdin_reader : public source<view>
{
public:
	explicit stdin_reader(FILE* file = stdin)
		: _file(file)
	{
	}

	~stdin_reader()
	{
		while (!_lines.empty())
		{
			char* line = _lines.pop_back();
			free(line);
		}
	}
	
protected:
	virtual action process(view& v)
	{
		size_t size = 0;
		char* ptr = nullptr;
		if (-1 != getline(&ptr, &size, _file))
		{
			size = strlen(ptr) - 1;
			v.assign(ptr, size);
			
			_lines.push_back(ptr);
			return PASS_DOWNSTREAM;
		}
		else
		{
			// man says to free the buffer even if getline fails
			free(ptr);
		}

		return TERMINATE;
	}

private:
	list<char*> _lines;
	FILE* _file;
};

// reads stdin backwards
class reverse_stdin_reader : public source<view>
{
public:
	reverse_stdin_reader(FILE* file = stdin)
		: _done_reading_input(false) 
		, _file(file)
	{
	}

	~reverse_stdin_reader()
	{
		while (!_lines.empty())
		{
			char* line = _lines.pop_back();
			free(line);
		}
	}
	
protected:
	virtual action process(view& v)
	{
		if (!_done_reading_input)
		{
			accumulate_input();
			_done_reading_input = true;
		}
		if (!_lines.empty())
		{
			char* ptr = _lines.pop_front();

			// remove the trailing newline
			size_t len = strlen(ptr) - 1;
			std::cout << strlen(ptr) << std::endl;
			v.assign(ptr, len);
			return PASS_DOWNSTREAM;
		}

		return TERMINATE;
	}

private:
	void accumulate_input()
	{
		size_t size = 0;
		char* ptr = nullptr;
		while ((ptr = nullptr, size = 0) || -1 != getline(&ptr, &size, _file))
		{
			_lines.push_front(ptr);
		}
		// man says to free the buffer even when the call fails 
		free(ptr);
	}
	
	list<char*> _lines;
	bool _done_reading_input;
	FILE* _file;
};
} //namespace perg


