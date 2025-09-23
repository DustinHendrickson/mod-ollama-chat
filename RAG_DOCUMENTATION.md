# RAG (Retrieval-Augmented Generation) Feature Documentation

## Overview

The RAG feature enhances the mod-ollama-chat module by providing bots with relevant contextual information from a knowledge base when responding to player messages. This allows bots to give more accurate, detailed, and informative responses about World of Warcraft game mechanics, professions, classes, and other topics.

## How It Works

1. **Knowledge Base**: JSON files containing structured information about WoW topics
2. **Query Analysis**: When a player sends a message, the system analyzes it for relevant keywords
3. **Information Retrieval**: The system searches the knowledge base for related information
4. **Prompt Enhancement**: Retrieved information is added to the bot's prompt before generating a response
5. **Contextual Responses**: Bots can now provide detailed information about professions, classes, etc.

## Configuration

### Basic Settings

```properties
# Enable/disable the RAG feature
OllamaChat.EnableRAG = 1

# Path to RAG data files (relative to module data directory)
OllamaChat.RAGDataPath = rag/

# Maximum number of information items to retrieve
OllamaChat.RAGMaxRetrievedItems = 3

# Minimum similarity score for information to be considered relevant (0.0-1.0)
OllamaChat.RAGSimilarityThreshold = 0.3

# Template for including RAG information in prompts
OllamaChat.RAGPromptTemplate = "RELEVANT INFORMATION:\n{rag_info}\nUse this information to provide accurate and detailed responses when applicable."
```

## Data Format

RAG data is stored in JSON files in the `data/rag/` directory. Each file contains an array of entries with the following structure:

```json
[
  {
    "id": "unique_identifier",
    "title": "Display Title",
    "content": "Detailed information about the topic",
    "keywords": ["keyword1", "keyword2", "keyword3"],
    "tags": ["tag1", "tag2"]
  }
]
```

### Field Descriptions

- **id**: Unique identifier for the entry (used internally)
- **title**: Human-readable title for the information
- **content**: The detailed information that will be provided to the bot
- **keywords**: Array of keywords that help match queries to this information
- **tags**: Array of category tags for organization

## Example Usage

### Player Message: "How do I become an alchemist?"
**Without RAG**: Bot gives a generic response about professions
**With RAG**: Bot provides detailed information about Alchemy, required skills, training locations, and key recipes

### Player Message: "What's the best class for raiding?"
**Without RAG**: Generic class advice
**With RAG**: Detailed information about different classes, their roles, strengths, and raid viability

## Creating Custom Knowledge Base

1. Create JSON files in `data/rag/` directory
2. Follow the JSON structure above
3. Use relevant keywords that players might use in queries
4. Keep content concise but informative
5. Test with various query patterns

### Tips for Good Keywords

- Include common misspellings
- Use both singular and plural forms
- Include related terms (e.g., "potions" for alchemy)
- Consider how players actually phrase questions

## Performance Considerations

- **Memory Usage**: All RAG data is loaded into memory on startup
- **Query Speed**: Retrieval uses simple text similarity, very fast
- **Token Limits**: Retrieved information adds to prompt length
- **Relevance Filtering**: Similarity threshold prevents irrelevant information

## Troubleshooting

### RAG Not Working

1. Check that `OllamaChat.EnableRAG = 1`
2. Verify JSON files exist in `data/rag/` directory
3. Check server logs for RAG initialization messages
4. Ensure JSON syntax is valid

### Poor Relevance

1. Adjust `OllamaChat.RAGSimilarityThreshold` (lower = more results, higher = fewer but more relevant)
2. Add more keywords to your JSON entries
3. Review query preprocessing logic

### Performance Issues

1. Reduce `OllamaChat.RAGMaxRetrievedItems`
2. Increase `OllamaChat.RAGSimilarityThreshold`
3. Optimize JSON file sizes

## Examples

### Professions Data
See `wow_professions.json` for examples of profession information including:
- Alchemy (potions, elixirs, flasks)
- Blacksmithing (weapons, armor, tools)
- Enchanting (disenchanting, enchants)
- And more...

### Classes and Factions Data
See `wow_classes_factions.json` for examples of:
- Class descriptions and roles
- Faction information and races
- Specializations and abilities

## Future Enhancements

- Support for more advanced similarity algorithms
- Dynamic knowledge base updates
- Integration with external knowledge sources
- Multi-language support
- Category-based filtering