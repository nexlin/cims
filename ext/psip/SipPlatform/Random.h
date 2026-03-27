#ifndef _RANDOM_H_
#define _RANDOM_H_


class CRandom
{
public:
	CRandom();
	~CRandom();

	int Get();

#ifndef WIN32
private:
	unsigned int m_iSeed;
#endif
};

int RandomGet();

#endif
