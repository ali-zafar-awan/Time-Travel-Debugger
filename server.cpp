//======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)

#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include "helper.h"
using namespace std;

// ---- Constants ----
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // str + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;              // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                  // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
  struct Node
  {
    T data;
    Node *next;
    Node(const T &val) : data(val), next(nullptr) {}
  };
  Node *top;
  int32_t count;

public:
  Stack() : top(nullptr), count(0) {}

  ~Stack()
  {
    while (!isEmpty())
    {
      pop();
    }
  }

  void push(const T &val)
  {
    if (count == MAX_STACK_DEPTH)
    {
      throw overflow_error("Stack is full.\n");
    }
    Node *temp = new Node(val);
    temp->next = top;
    top = temp;
    count++;
  }

  T pop()
  {
    if (isEmpty())
    {
      throw underflow_error("Stack is empty.\n");
    }
    T val = top->data;
    Node *temp = top;
    top = top->next;
    delete temp;
    count--;
    return val;
  }

  T &peek() const
  {
    if (isEmpty())
    {
      throw underflow_error("Stack is empty.\n");
    }
    return top->data;
  }

  bool isEmpty() const { return count == 0; }

  int32_t depth() const { return count; }

  int32_t snapshot_into(T out[], int32_t maxLen) const
  {
    int32_t ct = 0;
    Node *temp = top;
    while (temp != nullptr && ct < maxLen)
    {
      out[ct] = temp->data;
      temp = temp->next;
      ct++;
    }
    return ct;
  }

  Stack(const Stack &) = delete;

  Stack &operator=(const Stack &) = delete;
};

// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;

struct TimelineNode
{
  Snapshot *data;
  TimelineNode *next;
  TimelineNode *prev;
  TimelineNode(Snapshot *d) : data(d), next(nullptr), prev(nullptr) {}
};

class Timeline
{
  TimelineNode *head, *tail;
  int32_t stepCount;

public:
  // Implement these functions
  Timeline() : head(nullptr), tail(nullptr), stepCount(0) {}
  Timeline(const Timeline &) = delete;
  Timeline &operator=(const Timeline &) = delete;
  ~Timeline();
  void record(Snapshot *s)
  {
    if (stepCount == 0)
    {
      head = tail = new TimelineNode(s);
      stepCount++;
      return;
    }
    TimelineNode *t = new TimelineNode(s);
    tail->next = t;
    t->prev = tail;
    tail = t;
    stepCount++;
  }
  TimelineNode *begin() const { return head; }
  int32_t getStepCount() const { return stepCount; }
};


Timeline::~Timeline()
{
  TimelineNode *temp = head;
  while (temp != nullptr)
  {
    TimelineNode *i = temp->next;
    delete temp->data;
    delete temp;
    temp = i;
  }
}

void writeHeader(FILE *f, const TTDBHeader &h)
{
  fwrite(h.magic, 1, 4, f);
  fwrite(&h.version, sizeof(int32_t), 1, f);
  fwrite(&h.stepCount, sizeof(int32_t), 1, f);
  fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
}

// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream &in, string &out)
{
  string line;

  while (getline(in, line))
  {
    if (!line.empty() && line.back() == '\r')
    {
      line.pop_back();
    }
    bool blank = true;

    for (char ch : line)
    {
      if (ch != ' ' && ch != '\t')
      {
        blank = false;
        break;
      }
    }

    if (!blank)
    {
      out = line;
      return true;
    }
  }

  return false;
}

string firstWord(const string &line)
{
  size_t i = 0;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
  {
    i++;
  }
  size_t start = i;
  while (i < line.size() && line[i] != ' ' && line[i] != '\t')
  {
    i++;
  }
  return line.substr(start, i - start);
}

string secondWord(const string &line)
{
  size_t i = 0;
  for (int w = 0; w < 2; w++)
  {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
    {
      i++;
    }
    if (i >= line.size())
    {
      return "";
    }
    size_t start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t')
    {
      i++;
    }
    if (w == 1)
    {
      return line.substr(start, i - start);
    }
  }
  return "";
}

bool validateProgram(const char *sourcePath)
{
  ifstream in(sourcePath);
  if (!in)
  {
    return false;
  }

  bool flag = false;
  string line;

  while (readSourceLine(in, line))
  {
    string s = firstWord(line);

    if (s == "func")
    {
      if (flag || secondWord(line).empty())
      {
        return false;
      }
      flag = true;
    }
    else if (s == "func_end")
    {
      if (!flag)
      {
        return false;
      }
      flag = false;
    }
  }

  return !flag;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
{
  int64_t pos = ftell(f);
  if (pos < 0)
  {
    throw runtime_error("ftell failed in writeResolveRecord");
  }

  int32_t s = (int32_t)text.size();

  if (fwrite(&offsetField, sizeof(int64_t), 1, f) != 1)
  {
    throw runtime_error("failed writing offset field");
  }
  if (fwrite(&s, sizeof(int32_t), 1, f) != 1)
  {
    throw runtime_error("failed writing size field");
  }
  if (s > 0 && fwrite(text.data(), 1, text.size(), f) != text.size())
  {
    throw runtime_error("failed writing record text");
  }

  return pos;
}

int64_t readResolveRecord(FILE *f, string &outText)
{
  int64_t offsetField = 0;
  int32_t sz = 0;

  if (fread(&offsetField, sizeof(int64_t), 1, f) != 1)
  {
    throw runtime_error("short read on offset field");
  }
  if (fread(&sz, sizeof(int32_t), 1, f) != 1)
  {
    throw runtime_error("short read on size field");
  }
  outText.resize(sz);
  if (sz > 0 && fread(&outText[0], 1, outText.size(), f) != outText.size())
  {
    throw runtime_error("short read on record text");
  }

  return offsetField;
}

int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
  FuncEntry funcArray[MAX_FUNCS];
  int32_t funcCount = 0;
  PendingPatch patches[MAX_PATCHES];
  int32_t patchCount = 0;

  ifstream in(sourcePath);
  if (!in)
  {
    throw runtime_error("cannot open source file");
  }
  FILE *f = fopen(resolveBinPath, "w+b");
  if (f == nullptr)
  {
    throw runtime_error("cannot create resolve.bin");
  }

  try
  {
    string line;
    while (readSourceLine(in, line))
    {
      string s = firstWord(line);
      int64_t p = writeResolveRecord(f, -1, line);

      if (s == "func")
      {
        string name = secondWord(line);
        for (int32_t i = 0; i < funcCount; i++)
        {
          if (funcArray[i].funcName == name)
          {
            fclose(f);
            return -1;
          }
        }
        if (funcCount == MAX_FUNCS)
        {
          throw runtime_error("too many functions");
        }
        funcArray[funcCount].funcName = name;
        funcArray[funcCount].byteOffsetInResolveBin = p;
        funcCount++;
      }
      else if (s == "call")
      {
        if (patchCount == MAX_PATCHES)
        {
          throw runtime_error("too many call sites");
        }
        patches[patchCount].byteOffsetOfOffsetField = p;
        patches[patchCount].targetFuncName = secondWord(line);
        patchCount++;
      }
    }

    int64_t mainOffset = -1;
    for (int32_t i = 0; i < funcCount; i++)
    {
      if (funcArray[i].funcName == "main")
      {
        mainOffset = funcArray[i].byteOffsetInResolveBin;
        break;
      }
    }
    if (mainOffset < 0)
    {
      fclose(f);
      return -1;
    }

    for (int32_t j = 0; j < patchCount; j++)
    {
      int64_t target = -1;
      for (int32_t i = 0; i < funcCount; i++)
      {
        if (funcArray[i].funcName == patches[j].targetFuncName)
        {
          target = funcArray[i].byteOffsetInResolveBin;
          break;
        }
      }
      if (target < 0)
      {
        fclose(f);
        return -1;
      }
      if (fseek(f, (long)patches[j].byteOffsetOfOffsetField, SEEK_SET) != 0)
      {
        throw runtime_error("fseek failed while patching");
      }
      if (fwrite(&target, sizeof(int64_t), 1, f) != 1)
      {
        throw runtime_error("failed writing patched offset");
      }
    }

    fclose(f);
    return mainOffset;
  }
  catch (...)
  {
    fclose(f);
    throw;
  }
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
  KEYWORD,
  IDENTIFIER,
  PARAM
};

struct Token
{
  TokenType type;
  string text;
};

int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
  int32_t n = 0;
  size_t i = 0;

  while (true)
  {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
    {
      i++;
    }
    if (i >= line.size())
    {
      break;
    }
    size_t start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t')
    {
      i++;
    }

    if (n >= maxTokens)
    {
      throw runtime_error("too many tokens on one line");
    }
    string word = line.substr(start, i - start);

    if (n == 0)
    {
      if (word != "func" && word != "func_end" && word != "call" && word != "set" && word != "add" && word != "sub" && word != "mul" && word != "div")
      {
        throw runtime_error("unknown keyword: " + word);
      }
      tokens[n].type = KEYWORD;
    }
    else if (n == 1)
    {
      tokens[n].type = IDENTIFIER;
    }
    else
    {
      tokens[n].type = PARAM;
    }
    tokens[n].text = word;
    n++;
  }

  return n;
}

Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
  Snapshot *s = new Snapshot();
  s->stackDepth = callStack.snapshot_into(s->callStack, MAX_STACK_DEPTH);
  return s;
}

Variable *findVar(Frame &fr, const string &name)
{
  for (int32_t i = 0; i < fr.argc; i++)
  {
    if (fr.argv[i].name == name)
    {
      return &fr.argv[i];
    }
  }
  for (int32_t i = 0; i < fr.localCount; i++)
  {
    if (fr.locals[i].name == name)
    {
      return &fr.locals[i];
    }
  }
  return nullptr;
}

int32_t getValue(Frame &fr, const string &txt)
{
  size_t st = 0;
  if (txt[0] == '-')
  {
    st = 1;
  }
  bool flag = st < txt.size();
  for (size_t j = st; j < txt.size(); j++)
  {
    if (txt[j] < '0' || txt[j] > '9')
    {
      flag = false;
      break;
    }
  }
  if (flag)
  {
    return (int32_t)stoll(txt);
  }
  Variable *var = findVar(fr, txt);
  if (var == nullptr)
  {
    throw runtime_error("undefined variable: " + txt);
  }
  return var->value;
}

void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
  FILE *f = fopen(resolveBinPath, "rb");
  if (f == nullptr)
  {
    throw runtime_error("cannot open resolve.bin");
  }

  Stack<Frame> callStack;
  string refNames[MAX_STACK_DEPTH][MAX_VARS_PER_FRAME];
  Token tokens[MAX_TOKENS];
  Token ht[MAX_TOKENS];
  string text, htext;

  fseek(f, (long)mainOffset, SEEK_SET);
  readResolveRecord(f, htext);
  int32_t hn = tokenizeLine(htext, ht, MAX_TOKENS);
  Frame mainFrame{};
  mainFrame.func_name = ht[1].text;
  mainFrame.argc = hn - 2;
  for (int32_t i = 0; i < mainFrame.argc; i++)
  {
    mainFrame.argv[i].name = ht[2 + i].text;
  }
  mainFrame.returnLine = -1;
  callStack.push(mainFrame);

  while (!callStack.isEmpty())
  {
    int64_t offsetField = readResolveRecord(f, text);
    int32_t n = tokenizeLine(text, tokens, MAX_TOKENS);
    const string &str = tokens[0].text;
    Frame &fr = callStack.peek();

    if (str == "set")
    {
      if (n != 3)
      {
        throw runtime_error("set needs a name and one value");
      }
      int32_t v = getValue(fr, tokens[2].text);
      Variable *var = findVar(fr, tokens[1].text);
      if (var == nullptr)
      {
        if (fr.localCount == MAX_VARS_PER_FRAME)
        {
          throw runtime_error("too many locals");
        }
        var = &fr.locals[fr.localCount++];
        var->name = tokens[1].text;
      }
      var->value = v;
    }
    else if (str == "add" || str == "sub" || str == "mul" || str == "div")
    {
      if (n < 3)
      {
        throw runtime_error(str + " needs a target and an operand");
      }
      Variable *dest = findVar(fr, tokens[1].text);
      if (dest == nullptr)
      {
        throw runtime_error("undefined variable: " + tokens[1].text);
      }
      int64_t r = dest->value;
      for (int32_t i = 2; i < n; i++)
      {
        int64_t x = getValue(fr, tokens[i].text);
        if (str == "add")
        {
          r += x;
        }
        else if (str == "sub")
        {
          r -= x;
        }
        else if (str == "mul")
        {
          r *= x;
        }
        else
        {
          if (x == 0)
          {
            throw runtime_error("division by zero");
          }
          r /= x;
        }
      }
      dest->value = (int32_t)r;
    }
    else if (str == "call")
    {
      long rp = ftell(f);
      fseek(f, (long)offsetField, SEEK_SET);
      readResolveRecord(f, htext);
      hn = tokenizeLine(htext, ht, MAX_TOKENS);
      if (hn != n)
      {
        throw runtime_error("wrong argument count in call");
      }

      Frame callee{};
      callee.func_name = ht[1].text;
      callee.argc = hn - 2;
      for (int32_t i = 0; i < callee.argc; i++)
      {
        callee.argv[i].name = ht[2 + i].text;
        callee.argv[i].value = getValue(fr, tokens[2 + i].text);
      }
      callee.returnLine = (int32_t)rp;

      int32_t pos = callStack.depth();
      callStack.push(callee);
      for (int32_t i = 0; i < callee.argc; i++)
      {
        refNames[pos][i] = tokens[2 + i].text;
      }
    }
    else if (str == "func_end")
    {
      Frame done = callStack.pop();
      if (!callStack.isEmpty())
      {
        Frame &caller = callStack.peek();
        int32_t pos = callStack.depth();
        for (int32_t i = 0; i < done.argc; i++)
        {
          Variable *dst = findVar(caller, refNames[pos][i]);
          if (dst != nullptr)
          {
            dst->value = done.argv[i].value;
          }
        }
        fseek(f, (long)done.returnLine, SEEK_SET);
      }
    }

    if (!callStack.isEmpty())
    {
      timeline.record(buildSnapshot(callStack));
    }
  }

  fclose(f);
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
  FILE *f = fopen(tdbgPath, "wb");
  if (f == nullptr)
  {
    throw runtime_error("cannot create session.tdbg");
  }

  int32_t count = timeline.getStepCount();
  int64_t *index = new int64_t[count];
  int32_t len = 0;

  TTDBHeader h{};
  h.magic[0] = 'T';
  h.magic[1] = 'T';
  h.magic[2] = 'D';
  h.magic[3] = 'B';
  h.version = 1;
  h.stepCount = count;
  h.indexOffset = 0;
  writeHeader(f, h);

  TimelineNode *node = timeline.begin();
  for (int32_t i = 0; i < count; i++)
  {
    index[i] = ftell(f);
    const Snapshot &s = *node->data;

    fwrite(&s.stackDepth, sizeof(int32_t), 1, f);
    for (int32_t d = 0; d < s.stackDepth; d++)
    {
      const Frame &fr = s.callStack[d];

      len = (int32_t)fr.func_name.size();
      fwrite(&len, sizeof(int32_t), 1, f);
      fwrite(fr.func_name.data(), 1, fr.func_name.size(), f);

      fwrite(&fr.argc, sizeof(int32_t), 1, f);
      for (int32_t j = 0; j < fr.argc; j++)
      {
        len = (int32_t)fr.argv[j].name.size();
        fwrite(&len, sizeof(int32_t), 1, f);
        fwrite(fr.argv[j].name.data(), 1, fr.argv[j].name.size(), f);
        fwrite(&fr.argv[j].value, sizeof(int32_t), 1, f);
      }

      fwrite(&fr.returnLine, sizeof(int32_t), 1, f);

      fwrite(&fr.localCount, sizeof(int32_t), 1, f);
      for (int32_t j = 0; j < fr.localCount; j++)
      {
        len = (int32_t)fr.locals[j].name.size();
        fwrite(&len, sizeof(int32_t), 1, f);
        fwrite(fr.locals[j].name.data(), 1, fr.locals[j].name.size(), f);
        fwrite(&fr.locals[j].value, sizeof(int32_t), 1, f);
      }
    }
    node = node->next;
  }

  h.indexOffset = ftell(f);
  fwrite(index, sizeof(int64_t), count, f);

  bool failed = false;
  if (fseek(f, 0, SEEK_SET) != 0)
  {
    failed = true;
  }
  else
  {
    writeHeader(f, h);
  }

  if (ferror(f) != 0)
  {
    failed = true;
  }
  if (fclose(f) != 0)
  {
    failed = true;
  }
  delete[] index;

  if (failed)
  {
    throw runtime_error("failed writing session.tdbg");
  }
}

int32_t main()
{
  try
  {
    if (!validateProgram("source.bin"))
    {
      cerr << "Error: invalid program (func/func_end mismatch or nested func)" << endl;
      return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");
    if (mainOffset < 0)
    {
      cerr << "Error: no main, duplicate function, or call to an undefined function" << endl;
      return 1;
    }

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);
    writeTdbg(timeline, "session.tdbg");
  }
  catch (const exception &e)
  {
    cerr << "Error: " << e.what() << endl;
    return 1;
  }

  return 0;
}