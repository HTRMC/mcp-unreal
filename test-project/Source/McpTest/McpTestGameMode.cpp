#include "McpTestGameMode.h"

#include "McpTestCharacter.h"

AMcpTestGameMode::AMcpTestGameMode()
{
	DefaultPawnClass = AMcpTestCharacter::StaticClass();
}
