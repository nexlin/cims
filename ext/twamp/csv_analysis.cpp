
#include <map>
#include <string>
#include <iostream>
#include <fstream>

#include <vector>

#include <string>
#include <fstream>
#include <vector>
#include <utility> // std::pair
#include <stdexcept> // std::runtime_error
#include <sstream> // std::stringstream
#include <algorithm>

class CSVReader
{
private:
   std::ifstream _file;

public:
   CSVReader() {}
   ~CSVReader() {close();}

   bool open(const std::string & fname)
   {
	  _file.open(fname);

	  return _file.is_open();
   }
   void close()
   {
	  _file.close();
   }

   int get(std::vector<std::string> & data)
   {
	  int count=0;
	  std::string line, column;

	  if(std::getline(_file, line)) 
	  {
		 //parser
		 //std::cout << "# line:" << line << std::endl;

		 std::stringstream ss(line);
		 while(std::getline(ss, column, ','))
		 {
			data.push_back(column);
			//printf("## column[%d] = %s\n", count, column.c_str());
			count++;
		 }
		 //std::cout << std::endl;
	  }

	  return count;
   }
};

class CAnalysisSum
{
private:
   std::map<std::string, double> _data;
   std::map<std::string, unsigned long> _count;
   std::vector<std::pair<double, std::string>> _sort;

public:
   CAnalysisSum() {}
   ~CAnalysisSum() {}

   bool put(const std::string & key, std::string data, std::string count)
   {

	  //printf("# %s, %s, %s\n", key.c_str(), data.c_str(), count.c_str());
	  if(_data.find(key) == _data.end()) 
	  {
		 _data[key] = 0; 
		 _count[key] = 0;
	  }

	  double val = std::stod(data);
	  int    n   = std::stoi(count);

	  _data[key] = _data[key] + (val * n);
	  _count[key] = _count[key] + n;
	  return true;
   }

   bool analysis()
   {
	  sort();
	  print(100);
   }
   bool sort()
   {
	  for(auto data : _data)
	  {
		 double val = data.second  / _count[data.first];
		 _sort.push_back(make_pair(val, data.first));
	  }

	  std::sort(_sort.begin(), _sort.end());
	  //std::sort(_sort.end(), _sort.begin());
	  return true;
   }

   bool print(int n)
   {
	  int count = _sort.size()>n ? n : _sort.size();
	  int index = _sort.size()-1; 
	  for(int i=0; i<count; i++, index--)
	  {
		 fprintf(stderr, "#[%04d] => KEY=%s : VALUE=%lf\n", i, _sort[index].second.c_str(), _sort[index].first); 
		 printf("%d, %s, %lf\n", i, _sort[index].second.c_str(), _sort[index].first); 
	  }
	  return true;
   }
};


int main(int argc, char ** argv) 
{
   // Read three_cols.csv and ones.csv

   CAnalysisSum _col4;
   
   CSVReader csv;   
   csv.open(argv[1]);


   int bSkip = true;

   if(bSkip) 
   {
	  std::vector<std::string> row; 
	  csv.get(row);
   }
   while(1)
   {
	  std::vector<std::string> row; 

	  int count = csv.get(row);
	  if(count == 0) break;

	  _col4.put(row[1], row[4], row[9]);

 #if 0 
	  for(int i=0; i<count; i++)
	  {
		 printf("* col[%d] = %s \n", i, row[i].c_str());
	  }
#endif
   }
   csv.close();
   _col4.analysis();

   return 0;
}

